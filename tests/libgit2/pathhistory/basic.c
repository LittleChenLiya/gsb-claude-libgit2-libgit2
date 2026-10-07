#include "clar_libgit2.h"
#include "git2/pathhistory.h"

static git_repository *g_repo;
static git_signature *g_sig;

/* blob contents */
static const char content_one[] = "line one\n";
static const char content_two[] = "line one\nline two\n";
static const char content_side[] = "line one\nline two\nside branch\n";
static const char content_merged[] = "line one\nline two\nside branch\nmerged\n";
static const char content_recreated[] = "recreated file\n";
static const char content_copy[] = "copy me\n copy me again\n";
static const unsigned char content_bin1[] = {
	0x00, 0x01, 0x02, 0x00, 0xff, 0xfe, 0x00, 0x62, 0x69, 0x6e, 0x00
};
static const unsigned char content_bin2[] = {
	0x00, 0x01, 0x02, 0x00, 0xff, 0xfe, 0x00, 0x62, 0x69, 0x6e, 0x00,
	0xaa, 0xbb, 0x00, 0xcc
};

static git_oid b_one, b_two, b_side, b_merged;
static git_oid b_recreated, b_copy, b_bin1, b_bin2;

/* commits of the main history: add, modify, rename, side change, merge */
static git_oid c_add, c_modify, c_rename, c_side, c_merge;

typedef struct {
	const char *name;
	const git_oid *blob;
} file_entry;

static void make_blob(git_oid *out, const void *data, size_t len)
{
	cl_git_pass(git_blob_create_from_buffer(out, g_repo, data, len));
}

static void make_commit(git_oid *out, const char *message,
	const file_entry *files, size_t nfiles,
	const git_oid *parents, size_t nparents)
{
	git_treebuilder *builder;
	git_tree *tree;
	git_oid tree_id;
	git_commit *parent_commits[8];
	size_t i;

	cl_git_pass(git_treebuilder_new(&builder, g_repo, NULL));
	for (i = 0; i < nfiles; i++)
		cl_git_pass(git_treebuilder_insert(
			NULL, builder, files[i].name, files[i].blob,
			GIT_FILEMODE_BLOB));
	cl_git_pass(git_treebuilder_write(&tree_id, builder));
	git_treebuilder_free(builder);

	cl_git_pass(git_tree_lookup(&tree, g_repo, &tree_id));

	cl_assert(nparents <= 8);
	for (i = 0; i < nparents; i++)
		cl_git_pass(git_commit_lookup(
			&parent_commits[i], g_repo, &parents[i]));

	cl_git_pass(git_commit_create(out, g_repo, NULL, g_sig, g_sig,
		NULL, message, tree, nparents,
		(const git_commit **)parent_commits));

	for (i = 0; i < nparents; i++)
		git_commit_free(parent_commits[i]);
	git_tree_free(tree);
}

void test_pathhistory_basic__initialize(void)
{
	git_oid parents[2];
	file_entry files[2];

	g_repo = cl_git_sandbox_init("empty_standard_repo");
	cl_git_pass(git_signature_new(
		&g_sig, "Tester", "tester@example.com", 1234567890, 0));

	make_blob(&b_one, content_one, strlen(content_one));
	make_blob(&b_two, content_two, strlen(content_two));
	make_blob(&b_side, content_side, strlen(content_side));
	make_blob(&b_merged, content_merged, strlen(content_merged));
	make_blob(&b_recreated, content_recreated, strlen(content_recreated));
	make_blob(&b_copy, content_copy, strlen(content_copy));
	make_blob(&b_bin1, content_bin1, sizeof(content_bin1));
	make_blob(&b_bin2, content_bin2, sizeof(content_bin2));

	/* c_add: add "file.txt" */
	files[0].name = "file.txt", files[0].blob = &b_one;
	make_commit(&c_add, "add file.txt", files, 1, NULL, 0);

	/* c_modify: modify "file.txt" */
	files[0].name = "file.txt", files[0].blob = &b_two;
	git_oid_cpy(&parents[0], &c_add);
	make_commit(&c_modify, "modify file.txt", files, 1, parents, 1);

	/* c_rename: rename "file.txt" to "renamed.txt" (same content) */
	files[0].name = "renamed.txt", files[0].blob = &b_two;
	git_oid_cpy(&parents[0], &c_modify);
	make_commit(&c_rename, "rename file.txt", files, 1, parents, 1);

	/* c_side: side branch off c_modify modifying "file.txt" */
	files[0].name = "file.txt", files[0].blob = &b_side;
	git_oid_cpy(&parents[0], &c_modify);
	make_commit(&c_side, "side change", files, 1, parents, 1);

	/* c_merge: merge c_rename and c_side */
	files[0].name = "renamed.txt", files[0].blob = &b_merged;
	git_oid_cpy(&parents[0], &c_rename);
	git_oid_cpy(&parents[1], &c_side);
	make_commit(&c_merge, "merge", files, 1, parents, 2);
}

void test_pathhistory_basic__cleanup(void)
{
	git_signature_free(g_sig);
	g_sig = NULL;
	cl_git_sandbox_cleanup();
}

/* --- entry recording ------------------------------------------------ */

typedef struct {
	git_path_history_entry_type_t type;
	git_oid commit_id;
	char path[256];
	char old_path[256];
	git_oid blob_id;
	git_oid old_blob_id;
	uint16_t similarity;
	unsigned int parent_index;
} recorded_entry;

static recorded_entry g_entries[64];
static size_t g_nentries;
static size_t g_cancel_after;

static int record_entry(
	const git_path_history_entry *entry, void *payload)
{
	recorded_entry *rec;

	GIT_UNUSED(payload);

	cl_assert(g_nentries < 64);
	rec = &g_entries[g_nentries++];

	memset(rec, 0, sizeof(*rec));
	rec->type = entry->type;
	git_oid_cpy(&rec->commit_id, &entry->commit_id);
	if (entry->path)
		strncpy(rec->path, entry->path, sizeof(rec->path) - 1);
	if (entry->old_path)
		strncpy(rec->old_path, entry->old_path, sizeof(rec->old_path) - 1);
	git_oid_cpy(&rec->blob_id, &entry->blob_id);
	git_oid_cpy(&rec->old_blob_id, &entry->old_blob_id);
	rec->similarity = entry->similarity;
	rec->parent_index = entry->parent_index;

	if (g_cancel_after && g_nentries >= g_cancel_after)
		return 1;

	return 0;
}

static void reset_recording(void)
{
	memset(g_entries, 0, sizeof(g_entries));
	g_nentries = 0;
	g_cancel_after = 0;
}

static void assert_blob_content(
	const git_oid *id, const void *expected, size_t len)
{
	git_blob *blob;

	cl_git_pass(git_blob_lookup(&blob, g_repo, id));
	cl_assert_equal_i((int)len, (int)git_blob_rawsize(blob));
	cl_assert(memcmp(git_blob_rawcontent(blob), expected, len) == 0);
	git_blob_free(blob);
}

/* --- tests ---------------------------------------------------------- */

void test_pathhistory_basic__first_parent(void)
{
	git_path_history_options opts = GIT_PATH_HISTORY_OPTIONS_INIT;

	reset_recording();
	opts.strategy = GIT_PATH_HISTORY_STRATEGY_FIRST_PARENT;

	cl_git_pass(git_path_history_foreach(
		g_repo, &c_merge, "renamed.txt", &opts, record_entry, NULL));

	cl_assert_equal_i(4, (int)g_nentries);

	/* merge commit modified renamed.txt (first parent edge) */
	cl_assert_equal_i(GIT_PATH_HISTORY_ENTRY_MODIFIED, g_entries[0].type);
	cl_assert(git_oid_equal(&g_entries[0].commit_id, &c_merge));
	cl_assert_equal_s("renamed.txt", g_entries[0].path);
	cl_assert(git_oid_equal(&g_entries[0].blob_id, &b_merged));
	cl_assert(git_oid_equal(&g_entries[0].old_blob_id, &b_two));
	cl_assert_equal_i(0, (int)g_entries[0].parent_index);
	assert_blob_content(&g_entries[0].blob_id,
		content_merged, strlen(content_merged));

	/* rename commit renamed file.txt to renamed.txt */
	cl_assert_equal_i(GIT_PATH_HISTORY_ENTRY_RENAMED, g_entries[1].type);
	cl_assert(git_oid_equal(&g_entries[1].commit_id, &c_rename));
	cl_assert_equal_s("renamed.txt", g_entries[1].path);
	cl_assert_equal_s("file.txt", g_entries[1].old_path);
	cl_assert(git_oid_equal(&g_entries[1].blob_id, &b_two));
	cl_assert(git_oid_equal(&g_entries[1].old_blob_id, &b_two));
	cl_assert_equal_i(100, g_entries[1].similarity);
	assert_blob_content(&g_entries[1].blob_id,
		content_two, strlen(content_two));

	/* modify commit changed file.txt */
	cl_assert_equal_i(GIT_PATH_HISTORY_ENTRY_MODIFIED, g_entries[2].type);
	cl_assert(git_oid_equal(&g_entries[2].commit_id, &c_modify));
	cl_assert_equal_s("file.txt", g_entries[2].path);
	cl_assert(git_oid_equal(&g_entries[2].blob_id, &b_two));
	cl_assert(git_oid_equal(&g_entries[2].old_blob_id, &b_one));
	assert_blob_content(&g_entries[2].blob_id,
		content_two, strlen(content_two));

	/* root commit added file.txt */
	cl_assert_equal_i(GIT_PATH_HISTORY_ENTRY_ADDED, g_entries[3].type);
	cl_assert(git_oid_equal(&g_entries[3].commit_id, &c_add));
	cl_assert_equal_s("file.txt", g_entries[3].path);
	cl_assert(git_oid_equal(&g_entries[3].blob_id, &b_one));
	assert_blob_content(&g_entries[3].blob_id,
		content_one, strlen(content_one));
}

void test_pathhistory_basic__all_parents(void)
{
	git_path_history_options opts = GIT_PATH_HISTORY_OPTIONS_INIT;

	reset_recording();
	opts.strategy = GIT_PATH_HISTORY_STRATEGY_ALL_PARENTS;

	cl_git_pass(git_path_history_foreach(
		g_repo, &c_merge, "renamed.txt", &opts, record_entry, NULL));

	/*
	 * the merge contributes two entries (one per parent edge); the
	 * shared (c_modify, file.txt) and (c_add, file.txt) states are
	 * only visited once even though both branches lead to them
	 */
	cl_assert_equal_i(6, (int)g_nentries);

	/* first parent edge of the merge: renamed.txt modified */
	cl_assert_equal_i(GIT_PATH_HISTORY_ENTRY_MODIFIED, g_entries[0].type);
	cl_assert(git_oid_equal(&g_entries[0].commit_id, &c_merge));
	cl_assert_equal_s("renamed.txt", g_entries[0].path);
	cl_assert_equal_i(0, (int)g_entries[0].parent_index);
	cl_assert(git_oid_equal(&g_entries[0].blob_id, &b_merged));
	cl_assert(git_oid_equal(&g_entries[0].old_blob_id, &b_two));

	/* second parent edge: renamed from the side branch file.txt */
	cl_assert_equal_i(GIT_PATH_HISTORY_ENTRY_RENAMED, g_entries[1].type);
	cl_assert(git_oid_equal(&g_entries[1].commit_id, &c_merge));
	cl_assert_equal_s("renamed.txt", g_entries[1].path);
	cl_assert_equal_s("file.txt", g_entries[1].old_path);
	cl_assert_equal_i(1, (int)g_entries[1].parent_index);
	cl_assert(git_oid_equal(&g_entries[1].blob_id, &b_merged));
	cl_assert(git_oid_equal(&g_entries[1].old_blob_id, &b_side));
	cl_assert(g_entries[1].similarity > 0);
	assert_blob_content(&g_entries[1].old_blob_id,
		content_side, strlen(content_side));

	/* first parent chain continues through the rename */
	cl_assert_equal_i(GIT_PATH_HISTORY_ENTRY_RENAMED, g_entries[2].type);
	cl_assert(git_oid_equal(&g_entries[2].commit_id, &c_rename));
	cl_assert_equal_s("renamed.txt", g_entries[2].path);
	cl_assert_equal_s("file.txt", g_entries[2].old_path);

	cl_assert_equal_i(GIT_PATH_HISTORY_ENTRY_MODIFIED, g_entries[3].type);
	cl_assert(git_oid_equal(&g_entries[3].commit_id, &c_modify));
	cl_assert_equal_s("file.txt", g_entries[3].path);

	cl_assert_equal_i(GIT_PATH_HISTORY_ENTRY_ADDED, g_entries[4].type);
	cl_assert(git_oid_equal(&g_entries[4].commit_id, &c_add));
	cl_assert_equal_s("file.txt", g_entries[4].path);

	/* the side branch is visited exactly once */
	cl_assert_equal_i(GIT_PATH_HISTORY_ENTRY_MODIFIED, g_entries[5].type);
	cl_assert(git_oid_equal(&g_entries[5].commit_id, &c_side));
	cl_assert_equal_s("file.txt", g_entries[5].path);
	cl_assert_equal_i(0, (int)g_entries[5].parent_index);
	cl_assert(git_oid_equal(&g_entries[5].blob_id, &b_side));
	cl_assert(git_oid_equal(&g_entries[5].old_blob_id, &b_two));
	assert_blob_content(&g_entries[5].blob_id,
		content_side, strlen(content_side));
}

void test_pathhistory_basic__deleted_then_recreated(void)
{
	git_path_history_options opts = GIT_PATH_HISTORY_OPTIONS_INIT;
	git_oid parents[1], c_deleted, c_recreated;
	file_entry files[1];

	/* delete renamed.txt on top of the main history */
	git_oid_cpy(&parents[0], &c_merge);
	make_commit(&c_deleted, "delete renamed.txt", NULL, 0, parents, 1);

	/* recreate a file with the same name but new content */
	files[0].name = "renamed.txt", files[0].blob = &b_recreated;
	git_oid_cpy(&parents[0], &c_deleted);
	make_commit(&c_recreated, "recreate renamed.txt", files, 1, parents, 1);

	reset_recording();
	cl_git_pass(git_path_history_foreach(
		g_repo, &c_recreated, "renamed.txt", &opts,
		record_entry, NULL));

	/*
	 * the history stops at the recreation; it must not cross the
	 * deletion and reach the original "add file.txt" commit
	 */
	cl_assert_equal_i(1, (int)g_nentries);
	cl_assert_equal_i(GIT_PATH_HISTORY_ENTRY_ADDED, g_entries[0].type);
	cl_assert(git_oid_equal(&g_entries[0].commit_id, &c_recreated));
	cl_assert_equal_s("renamed.txt", g_entries[0].path);
	cl_assert(git_oid_equal(&g_entries[0].blob_id, &b_recreated));
	assert_blob_content(&g_entries[0].blob_id,
		content_recreated, strlen(content_recreated));
}

void test_pathhistory_basic__pure_copy_is_not_followed(void)
{
	git_path_history_options opts = GIT_PATH_HISTORY_OPTIONS_INIT;
	git_oid parents[1], c_orig, c_copy;
	file_entry files[2];

	files[0].name = "orig.txt", files[0].blob = &b_copy;
	make_commit(&c_orig, "add orig.txt", files, 1, NULL, 0);

	/* copy orig.txt to dup.txt, leaving the source untouched */
	files[0].name = "orig.txt", files[0].blob = &b_copy;
	files[1].name = "dup.txt", files[1].blob = &b_copy;
	git_oid_cpy(&parents[0], &c_orig);
	make_commit(&c_copy, "copy orig.txt to dup.txt", files, 2, parents, 1);

	reset_recording();
	cl_git_pass(git_path_history_foreach(
		g_repo, &c_copy, "dup.txt", &opts, record_entry, NULL));

	/*
	 * the copy is reported separately from a rename, and the walk
	 * does not continue along the copy source: the "add orig.txt"
	 * commit must not show up
	 */
	cl_assert_equal_i(1, (int)g_nentries);
	cl_assert_equal_i(GIT_PATH_HISTORY_ENTRY_COPIED, g_entries[0].type);
	cl_assert(git_oid_equal(&g_entries[0].commit_id, &c_copy));
	cl_assert_equal_s("dup.txt", g_entries[0].path);
	cl_assert_equal_s("orig.txt", g_entries[0].old_path);
	cl_assert(git_oid_equal(&g_entries[0].blob_id, &b_copy));
	cl_assert(git_oid_equal(&g_entries[0].old_blob_id, &b_copy));
	cl_assert_equal_i(100, g_entries[0].similarity);
}

void test_pathhistory_basic__binary_files(void)
{
	git_path_history_options opts = GIT_PATH_HISTORY_OPTIONS_INIT;
	git_oid parents[1], c_bin1, c_bin2, c_bin3;
	file_entry files[1];

	files[0].name = "data.old", files[0].blob = &b_bin1;
	make_commit(&c_bin1, "add binary", files, 1, NULL, 0);

	files[0].name = "data.new", files[0].blob = &b_bin1;
	git_oid_cpy(&parents[0], &c_bin1);
	make_commit(&c_bin2, "rename binary", files, 1, parents, 1);

	files[0].name = "data.new", files[0].blob = &b_bin2;
	git_oid_cpy(&parents[0], &c_bin2);
	make_commit(&c_bin3, "modify binary", files, 1, parents, 1);

	reset_recording();
	cl_git_pass(git_path_history_foreach(
		g_repo, &c_bin3, "data.new", &opts, record_entry, NULL));

	cl_assert_equal_i(3, (int)g_nentries);

	cl_assert_equal_i(GIT_PATH_HISTORY_ENTRY_MODIFIED, g_entries[0].type);
	cl_assert(git_oid_equal(&g_entries[0].commit_id, &c_bin3));
	cl_assert_equal_s("data.new", g_entries[0].path);
	cl_assert(git_oid_equal(&g_entries[0].blob_id, &b_bin2));
	cl_assert(git_oid_equal(&g_entries[0].old_blob_id, &b_bin1));
	assert_blob_content(&g_entries[0].blob_id,
		content_bin2, sizeof(content_bin2));

	cl_assert_equal_i(GIT_PATH_HISTORY_ENTRY_RENAMED, g_entries[1].type);
	cl_assert(git_oid_equal(&g_entries[1].commit_id, &c_bin2));
	cl_assert_equal_s("data.new", g_entries[1].path);
	cl_assert_equal_s("data.old", g_entries[1].old_path);
	cl_assert_equal_i(100, g_entries[1].similarity);
	cl_assert(git_oid_equal(&g_entries[1].blob_id, &b_bin1));
	assert_blob_content(&g_entries[1].blob_id,
		content_bin1, sizeof(content_bin1));

	cl_assert_equal_i(GIT_PATH_HISTORY_ENTRY_ADDED, g_entries[2].type);
	cl_assert(git_oid_equal(&g_entries[2].commit_id, &c_bin1));
	cl_assert_equal_s("data.old", g_entries[2].path);
	cl_assert(git_oid_equal(&g_entries[2].blob_id, &b_bin1));
}

void test_pathhistory_basic__max_entries_marks_truncated_result(void)
{
	git_path_history_options opts = GIT_PATH_HISTORY_OPTIONS_INIT;

	/* no limit: the full history is reported, return 0 */
	reset_recording();
	cl_git_pass(git_path_history_foreach(
		g_repo, &c_merge, "renamed.txt", &opts, record_entry, NULL));
	cl_assert_equal_i(4, (int)g_nentries);

	/* generous limit: still complete, return 0 */
	reset_recording();
	opts.max_entries = 16;
	cl_git_pass(git_path_history_foreach(
		g_repo, &c_merge, "renamed.txt", &opts, record_entry, NULL));
	cl_assert_equal_i(4, (int)g_nentries);

	/* tight limit: truncated result is flagged with return value 1 */
	reset_recording();
	opts.max_entries = 2;
	cl_assert_equal_i(1, git_path_history_foreach(
		g_repo, &c_merge, "renamed.txt", &opts, record_entry, NULL));
	cl_assert_equal_i(2, (int)g_nentries);
	cl_assert_equal_i(GIT_PATH_HISTORY_ENTRY_MODIFIED, g_entries[0].type);
	cl_assert_equal_i(GIT_PATH_HISTORY_ENTRY_RENAMED, g_entries[1].type);

	/* the limit also applies to an all-parents walk */
	reset_recording();
	opts.strategy = GIT_PATH_HISTORY_STRATEGY_ALL_PARENTS;
	opts.max_entries = 3;
	cl_assert_equal_i(1, git_path_history_foreach(
		g_repo, &c_merge, "renamed.txt", &opts, record_entry, NULL));
	cl_assert_equal_i(3, (int)g_nentries);
}

void test_pathhistory_basic__cancel_stops_walk_and_releases(void)
{
	git_path_history_options opts = GIT_PATH_HISTORY_OPTIONS_INIT;

	reset_recording();
	g_cancel_after = 2;

	cl_assert_equal_i(GIT_EUSER, git_path_history_foreach(
		g_repo, &c_merge, "renamed.txt", &opts, record_entry, NULL));
	cl_assert_equal_i(2, (int)g_nentries);

	/* the walk is fully cleaned up and can be run again */
	reset_recording();
	cl_git_pass(git_path_history_foreach(
		g_repo, &c_merge, "renamed.txt", &opts, record_entry, NULL));
	cl_assert_equal_i(4, (int)g_nentries);
}

void test_pathhistory_basic__missing_start_path(void)
{
	git_path_history_options opts = GIT_PATH_HISTORY_OPTIONS_INIT;

	reset_recording();
	cl_assert_equal_i(GIT_ENOTFOUND, git_path_history_foreach(
		g_repo, &c_merge, "does-not-exist.txt", &opts,
		record_entry, NULL));
	cl_assert_equal_i(0, (int)g_nentries);
}

void test_pathhistory_basic__default_options(void)
{
	reset_recording();

	/* NULL options follow the first parent only */
	cl_git_pass(git_path_history_foreach(
		g_repo, &c_merge, "renamed.txt", NULL, record_entry, NULL));
	cl_assert_equal_i(4, (int)g_nentries);
}
