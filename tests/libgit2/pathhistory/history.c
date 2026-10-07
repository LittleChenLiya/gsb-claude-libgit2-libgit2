#include "clar_libgit2.h"

#include "git2/pathhistory.h"

static git_repository *repo;
static git_signature *sig;

void test_pathhistory_history__initialize(void)
{
	repo = cl_git_sandbox_init_new("pathhistory");
	cl_git_pass(git_signature_new(&sig,
		"Path History", "ph@example.com", 1328377743, 0));
}

void test_pathhistory_history__cleanup(void)
{
	git_signature_free(sig);
	sig = NULL;
	cl_git_sandbox_cleanup();
}

static void write_file(const char *path, const char *content)
{
	git_str full = GIT_STR_INIT;

	cl_git_pass(git_str_joinpath(&full, git_repository_workdir(repo), path));
	cl_git_mkfile(full.ptr, content);
	git_str_dispose(&full);
}

static void write_file_raw(const char *path, const char *content, size_t len)
{
	git_str full = GIT_STR_INIT;
	FILE *fp;

	cl_git_pass(git_str_joinpath(&full, git_repository_workdir(repo), path));
	cl_assert((fp = fopen(full.ptr, "wb")) != NULL);
	cl_must_pass(fwrite(content, 1, len, fp) == len ? 0 : -1);
	cl_must_pass(fclose(fp));
	git_str_dispose(&full);
}

static void stage(const char *path)
{
	git_index *idx;

	cl_git_pass(git_repository_index(&idx, repo));
	cl_git_pass(git_index_add_bypath(idx, path));
	cl_git_pass(git_index_write(idx));
	git_index_free(idx);
}

static void rename_file(const char *old_path, const char *new_path)
{
	git_str old_full = GIT_STR_INIT, new_full = GIT_STR_INIT;
	git_index *idx;

	cl_git_pass(git_str_joinpath(&old_full, git_repository_workdir(repo), old_path));
	cl_git_pass(git_str_joinpath(&new_full, git_repository_workdir(repo), new_path));
	cl_must_pass(p_rename(old_full.ptr, new_full.ptr));

	cl_git_pass(git_repository_index(&idx, repo));
	cl_git_pass(git_index_remove_bypath(idx, old_path));
	cl_git_pass(git_index_add_bypath(idx, new_path));
	cl_git_pass(git_index_write(idx));
	git_index_free(idx);

	git_str_dispose(&old_full);
	git_str_dispose(&new_full);
}

static void delete_file(const char *path)
{
	git_str full = GIT_STR_INIT;
	git_index *idx;

	cl_git_pass(git_str_joinpath(&full, git_repository_workdir(repo), path));
	cl_must_pass(p_unlink(full.ptr));

	cl_git_pass(git_repository_index(&idx, repo));
	cl_git_pass(git_index_remove_bypath(idx, path));
	cl_git_pass(git_index_write(idx));
	git_index_free(idx);

	git_str_dispose(&full);
}

static git_oid commit_index(const char *message,
	const git_oid *parents, size_t parent_count)
{
	git_index *idx;
	git_oid tree_id, commit_id;
	git_tree *tree;
	git_commit *parent_commits[8];
	size_t i;

	cl_assert(parent_count <= 8);

	cl_git_pass(git_repository_index(&idx, repo));
	cl_git_pass(git_index_write_tree(&tree_id, idx));
	git_index_free(idx);
	cl_git_pass(git_tree_lookup(&tree, repo, &tree_id));

	for (i = 0; i < parent_count; i++)
		cl_git_pass(git_commit_lookup(&parent_commits[i], repo, &parents[i]));

	cl_git_pass(git_commit_create(&commit_id, repo, NULL, sig, sig,
		NULL, message, tree, parent_count,
		(const git_commit **)parent_commits));

	for (i = 0; i < parent_count; i++)
		git_commit_free(parent_commits[i]);
	git_tree_free(tree);

	return commit_id;
}

static void assert_blob_content(const git_oid *blob_id, const char *expected)
{
	git_blob *blob;

	cl_git_pass(git_blob_lookup(&blob, repo, blob_id));
	cl_assert_equal_i((int)strlen(expected), (int)git_blob_rawsize(blob));
	cl_assert(memcmp(expected, git_blob_rawcontent(blob),
		strlen(expected)) == 0);
	git_blob_free(blob);
}

static void assert_entry(const git_pathhistory_entry *entry,
	const git_oid *commit_id, const git_oid *parent_id,
	unsigned int parent_index, git_pathhistory_entry_type_t type,
	const char *path, const char *old_path)
{
	cl_assert_equal_oid(commit_id, &entry->commit_id);
	if (parent_id)
		cl_assert_equal_oid(parent_id, &entry->parent_id);
	cl_assert_equal_i((int)parent_index, (int)entry->parent_index);
	cl_assert_equal_i((int)type, (int)entry->type);
	cl_assert_equal_s(path, entry->path);
	if (old_path)
		cl_assert_equal_s(old_path, entry->old_path);
	else
		cl_assert(entry->old_path == NULL);
}

/*
 * Build a graph with an add, a modify, a rename and a two-parent merge:
 *
 *   c1: add a.txt ("one\n")
 *   c2: modify a.txt ("one\ntwo\n")
 *   cr (parent c2): rename a.txt -> b.txt
 *   cs (parent c2): modify a.txt ("one\ntwo\nthree\n")
 *   cm (parents cr, cs): b.txt = "one\ntwo\nthree\nmerged\n"
 */
static git_oid c1, c2, cr, cs, cm;

static void build_merge_graph(void)
{
	git_oid parents[2];

	write_file("a.txt", "one\n");
	stage("a.txt");
	c1 = commit_index("add a.txt", NULL, 0);

	write_file("a.txt", "one\ntwo\n");
	stage("a.txt");
	parents[0] = c1;
	c2 = commit_index("modify a.txt", parents, 1);

	rename_file("a.txt", "b.txt");
	parents[0] = c2;
	cr = commit_index("rename a.txt to b.txt", parents, 1);

	delete_file("b.txt");
	write_file("a.txt", "one\ntwo\nthree\n");
	stage("a.txt");
	parents[0] = c2;
	cs = commit_index("modify a.txt on side branch", parents, 1);

	delete_file("a.txt");
	write_file("b.txt", "one\ntwo\nthree\nmerged\n");
	stage("b.txt");
	parents[0] = cr;
	parents[1] = cs;
	cm = commit_index("merge side branch", parents, 2);
}

void test_pathhistory_history__first_parent(void)
{
	git_pathhistory *ph;
	git_pathhistory_entry entry;

	build_merge_graph();

	cl_git_pass(git_pathhistory_new(&ph, repo, &cm, "b.txt", NULL));
	cl_assert_equal_i(0, git_pathhistory_truncated(ph));

	cl_git_pass(git_pathhistory_next(&entry, ph));
	assert_entry(&entry, &cm, &cr, 0, GIT_PATHHISTORY_MODIFIED,
		"b.txt", NULL);
	assert_blob_content(&entry.blob_id, "one\ntwo\nthree\nmerged\n");

	cl_git_pass(git_pathhistory_next(&entry, ph));
	assert_entry(&entry, &cr, &c2, 0, GIT_PATHHISTORY_RENAMED,
		"b.txt", "a.txt");
	assert_blob_content(&entry.blob_id, "one\ntwo\n");
	assert_blob_content(&entry.old_blob_id, "one\ntwo\n");
	cl_assert_equal_i(100, entry.similarity);

	cl_git_pass(git_pathhistory_next(&entry, ph));
	assert_entry(&entry, &c2, &c1, 0, GIT_PATHHISTORY_MODIFIED,
		"a.txt", NULL);
	assert_blob_content(&entry.blob_id, "one\ntwo\n");

	cl_git_pass(git_pathhistory_next(&entry, ph));
	assert_entry(&entry, &c1, NULL, 0, GIT_PATHHISTORY_ADDED,
		"a.txt", NULL);
	assert_blob_content(&entry.blob_id, "one\n");

	cl_assert_equal_i(GIT_ITEROVER, git_pathhistory_next(&entry, ph));
	cl_assert_equal_i(0, git_pathhistory_truncated(ph));

	git_pathhistory_free(ph);
}

void test_pathhistory_history__all_parents(void)
{
	git_pathhistory_options opts = GIT_PATHHISTORY_OPTIONS_INIT;
	git_pathhistory *ph;
	git_pathhistory_entry entry;

	build_merge_graph();

	opts.flags = GIT_PATHHISTORY_ALL_PARENTS;

	cl_git_pass(git_pathhistory_new(&ph, repo, &cm, "b.txt", &opts));

	/* merge commit: edge to first parent (cr) is a modification */
	cl_git_pass(git_pathhistory_next(&entry, ph));
	assert_entry(&entry, &cm, &cr, 0, GIT_PATHHISTORY_MODIFIED,
		"b.txt", NULL);
	assert_blob_content(&entry.blob_id, "one\ntwo\nthree\nmerged\n");

	/* merge commit: edge to second parent (cs) is a rename */
	cl_git_pass(git_pathhistory_next(&entry, ph));
	assert_entry(&entry, &cm, &cs, 1, GIT_PATHHISTORY_RENAMED,
		"b.txt", "a.txt");
	assert_blob_content(&entry.blob_id, "one\ntwo\nthree\nmerged\n");
	assert_blob_content(&entry.old_blob_id, "one\ntwo\nthree\n");

	/* rename commit on the first-parent branch */
	cl_git_pass(git_pathhistory_next(&entry, ph));
	assert_entry(&entry, &cr, &c2, 0, GIT_PATHHISTORY_RENAMED,
		"b.txt", "a.txt");
	assert_blob_content(&entry.blob_id, "one\ntwo\n");

	/* modification on the side branch */
	cl_git_pass(git_pathhistory_next(&entry, ph));
	assert_entry(&entry, &cs, &c2, 0, GIT_PATHHISTORY_MODIFIED,
		"a.txt", NULL);
	assert_blob_content(&entry.blob_id, "one\ntwo\nthree\n");

	/* the shared ancestor state (c2, a.txt) is only visited once */
	cl_git_pass(git_pathhistory_next(&entry, ph));
	assert_entry(&entry, &c2, &c1, 0, GIT_PATHHISTORY_MODIFIED,
		"a.txt", NULL);
	assert_blob_content(&entry.blob_id, "one\ntwo\n");

	cl_git_pass(git_pathhistory_next(&entry, ph));
	assert_entry(&entry, &c1, NULL, 0, GIT_PATHHISTORY_ADDED,
		"a.txt", NULL);
	assert_blob_content(&entry.blob_id, "one\n");

	cl_assert_equal_i(GIT_ITEROVER, git_pathhistory_next(&entry, ph));
	cl_assert_equal_i(0, git_pathhistory_truncated(ph));

	git_pathhistory_free(ph);
}

void test_pathhistory_history__limit_marks_truncation(void)
{
	git_pathhistory_options opts = GIT_PATHHISTORY_OPTIONS_INIT;
	git_pathhistory *ph;
	git_pathhistory_entry entry;

	build_merge_graph();

	opts.flags = GIT_PATHHISTORY_ALL_PARENTS;
	opts.max_entries = 2;

	cl_git_pass(git_pathhistory_new(&ph, repo, &cm, "b.txt", &opts));

	cl_git_pass(git_pathhistory_next(&entry, ph));
	cl_assert_equal_i(0, entry.flags & GIT_PATHHISTORY_ENTRY_TRUNCATED);

	cl_git_pass(git_pathhistory_next(&entry, ph));
	cl_assert(entry.flags & GIT_PATHHISTORY_ENTRY_TRUNCATED);

	cl_assert_equal_i(GIT_ITEROVER, git_pathhistory_next(&entry, ph));
	cl_assert_equal_i(1, git_pathhistory_truncated(ph));

	git_pathhistory_free(ph);

	/* without a limit, the walk is not truncated */
	opts.max_entries = 0;
	cl_git_pass(git_pathhistory_new(&ph, repo, &cm, "b.txt", &opts));
	cl_assert_equal_i(0, git_pathhistory_truncated(ph));
	git_pathhistory_free(ph);
}

void test_pathhistory_history__delete_and_recreate(void)
{
	git_oid c_add, c_del, c_recreate;
	git_oid parents[1];
	git_pathhistory *ph;
	git_pathhistory_entry entry;

	write_file("d.txt", "first incarnation\n");
	stage("d.txt");
	c_add = commit_index("add d.txt", NULL, 0);

	delete_file("d.txt");
	parents[0] = c_add;
	c_del = commit_index("delete d.txt", parents, 1);

	write_file("d.txt", "second incarnation\n");
	stage("d.txt");
	parents[0] = c_del;
	c_recreate = commit_index("recreate d.txt", parents, 1);

	/* the walk must not cross the deletion into the old file */
	cl_git_pass(git_pathhistory_new(&ph, repo, &c_recreate, "d.txt", NULL));

	cl_git_pass(git_pathhistory_next(&entry, ph));
	assert_entry(&entry, &c_recreate, &c_del, 0, GIT_PATHHISTORY_ADDED,
		"d.txt", NULL);
	assert_blob_content(&entry.blob_id, "second incarnation\n");

	cl_assert_equal_i(GIT_ITEROVER, git_pathhistory_next(&entry, ph));

	git_pathhistory_free(ph);
}

void test_pathhistory_history__copy_not_followed(void)
{
	git_oid c_src, c_copy;
	git_oid parents[1];
	git_pathhistory *ph;
	git_pathhistory_entry entry;

	write_file("src.txt",
		"alpha beta gamma delta\n"
		"epsilon zeta eta theta\n"
		"iota kappa lambda mu\n");
	stage("src.txt");
	c_src = commit_index("add src.txt", NULL, 0);

	write_file("copy.txt",
		"alpha beta gamma delta\n"
		"epsilon zeta eta theta\n"
		"iota kappa lambda mu\n");
	stage("copy.txt");
	parents[0] = c_src;
	c_copy = commit_index("copy src.txt to copy.txt", parents, 1);

	cl_git_pass(git_pathhistory_new(&ph, repo, &c_copy, "copy.txt", NULL));

	cl_git_pass(git_pathhistory_next(&entry, ph));
	assert_entry(&entry, &c_copy, &c_src, 0, GIT_PATHHISTORY_COPIED,
		"copy.txt", "src.txt");
	cl_assert_equal_i(100, entry.similarity);

	/* the copy source must not be followed */
	cl_assert_equal_i(GIT_ITEROVER, git_pathhistory_next(&entry, ph));

	git_pathhistory_free(ph);
}

void test_pathhistory_history__binary_rename(void)
{
	git_oid c_add, c_ren;
	git_oid parents[1];
	git_pathhistory *ph;
	git_pathhistory_entry entry;
	git_blob *blob;
	char content[256];
	size_t i;

	for (i = 0; i < sizeof(content); i++)
		content[i] = (char)(i & 0xff);

	write_file_raw("bin.dat", content, sizeof(content));
	stage("bin.dat");
	c_add = commit_index("add bin.dat", NULL, 0);

	rename_file("bin.dat", "bin2.dat");
	parents[0] = c_add;
	c_ren = commit_index("rename bin.dat", parents, 1);

	cl_git_pass(git_pathhistory_new(&ph, repo, &c_ren, "bin2.dat", NULL));

	cl_git_pass(git_pathhistory_next(&entry, ph));
	assert_entry(&entry, &c_ren, &c_add, 0, GIT_PATHHISTORY_RENAMED,
		"bin2.dat", "bin.dat");
	cl_assert_equal_i(100, entry.similarity);

	cl_git_pass(git_pathhistory_next(&entry, ph));
	assert_entry(&entry, &c_add, NULL, 0, GIT_PATHHISTORY_ADDED,
		"bin.dat", NULL);

	cl_git_pass(git_blob_lookup(&blob, repo, &entry.blob_id));
	cl_assert_equal_i((int)sizeof(content), (int)git_blob_rawsize(blob));
	cl_assert(memcmp(content, git_blob_rawcontent(blob),
		sizeof(content)) == 0);
	git_blob_free(blob);

	cl_assert_equal_i(GIT_ITEROVER, git_pathhistory_next(&entry, ph));

	git_pathhistory_free(ph);
}

struct foreach_state {
	unsigned int calls;
	int cancel_after;
};

static int counting_cb(const git_pathhistory_entry *entry, void *payload)
{
	struct foreach_state *st = payload;

	GIT_UNUSED(entry);

	st->calls++;
	if (st->cancel_after > 0 && (int)st->calls >= st->cancel_after)
		return GIT_EUSER;

	return 0;
}

void test_pathhistory_history__foreach_and_cancel(void)
{
	git_pathhistory_options opts = GIT_PATHHISTORY_OPTIONS_INIT;
	struct foreach_state st;
	git_pathhistory *ph;
	git_pathhistory_entry entry;

	build_merge_graph();

	opts.flags = GIT_PATHHISTORY_ALL_PARENTS;

	/* full walk via callback */
	memset(&st, 0, sizeof(st));
	cl_git_pass(git_pathhistory_foreach(repo, &cm, "b.txt", &opts,
		counting_cb, &st));
	cl_assert_equal_i(6, (int)st.calls);

	/* canceled walk stops early and releases its resources */
	memset(&st, 0, sizeof(st));
	st.cancel_after = 3;
	cl_assert_equal_i(GIT_EUSER,
		git_pathhistory_foreach(repo, &cm, "b.txt", &opts,
			counting_cb, &st));
	cl_assert_equal_i(3, (int)st.calls);

	/* a partially consumed iterator can be freed */
	cl_git_pass(git_pathhistory_new(&ph, repo, &cm, "b.txt", &opts));
	cl_git_pass(git_pathhistory_next(&entry, ph));
	git_pathhistory_free(ph);
}

void test_pathhistory_history__missing_path(void)
{
	git_pathhistory *ph;

	build_merge_graph();

	cl_assert_equal_i(GIT_ENOTFOUND,
		git_pathhistory_new(&ph, repo, &cm, "no/such/file.txt", NULL));
	cl_assert_equal_i(GIT_ENOTFOUND,
		git_pathhistory_foreach(repo, &cm, "no/such/file.txt", NULL,
			counting_cb, NULL));
}
