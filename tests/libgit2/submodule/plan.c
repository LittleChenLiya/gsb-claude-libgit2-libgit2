/*
 * Tests for the read-only submodule update planner
 * (git_submodule_update_plan).
 */

#include "clar_libgit2.h"
#include "futils.h"
#include "git2/submodule_plan.h"

#include <time.h>

static git_signature *g_sig;

void test_submodule_plan__initialize(void)
{
	cl_git_pass(git_signature_new(
		&g_sig, "Planner Test", "planner@example.com", 1234567890, 0));
}

void test_submodule_plan__cleanup(void)
{
	git_signature_free(g_sig);
	g_sig = NULL;
	cl_git_sandbox_cleanup();
}

static git_repository *create_repo(const char *name)
{
	git_repository *repo = NULL;
	cl_git_pass(git_repository_init(&repo, name, 0));
	return repo;
}

static void write_file_in_repo(
	git_repository *repo, const char *filename, const char *content)
{
	git_str path = GIT_STR_INIT, buf = GIT_STR_INIT;

	cl_git_pass(git_str_joinpath(
		&path, git_repository_workdir(repo), filename));
	cl_git_pass(git_futils_mkpath2file(path.ptr, 0755));
	cl_git_pass(git_str_puts(&buf, content));
	cl_git_pass(git_futils_writebuffer(&buf, path.ptr, 0, 0644));

	git_str_dispose(&path);
	git_str_dispose(&buf);
}

/* commit all workdir files of a repo onto HEAD */
static void commit_file(
	git_repository *repo, const char *filename, const char *msg, git_oid *out)
{
	git_index *index;
	git_oid tree_id, parent_id;
	git_tree *tree;
	git_commit *parent = NULL;
	const git_commit *parents[1];
	size_t nparents = 0;

	cl_git_pass(git_repository_index(&index, repo));
	cl_git_pass(git_index_add_bypath(index, filename));
	cl_git_pass(git_index_write(index));
	cl_git_pass(git_index_write_tree(&tree_id, index));
	git_index_free(index);

	cl_git_pass(git_tree_lookup(&tree, repo, &tree_id));

	if (git_reference_name_to_id(&parent_id, repo, "HEAD") == 0) {
		cl_git_pass(git_commit_lookup(&parent, repo, &parent_id));
		parents[0] = parent;
		nparents = 1;
	} else {
		git_error_clear();
	}

	cl_git_pass(git_commit_create(
		out, repo, "HEAD", g_sig, g_sig, NULL, msg, tree, nparents, parents));

	git_commit_free(parent);
	git_tree_free(tree);
}

typedef struct {
	const char *path;
	git_oid target;
} test_gitlink;

/* build a commit directly from a .gitmodules string and gitlinks;
 * the workdir and index of the superproject are left untouched */
static void make_super_commit(
	git_repository *repo,
	const char *gitmodules,
	const test_gitlink *links,
	size_t nlinks,
	const git_oid *parent_id,
	git_oid *out)
{
	git_treebuilder *root = NULL;
	git_oid tree_id, blob_id;
	git_tree *tree;
	git_commit *parent = NULL;
	const git_commit *parents[1];
	size_t i;

	cl_git_pass(git_treebuilder_new(&root, repo, NULL));

	if (gitmodules) {
		cl_git_pass(git_blob_create_from_buffer(
			&blob_id, repo, gitmodules, strlen(gitmodules)));
		cl_git_pass(git_treebuilder_insert(
			NULL, root, ".gitmodules", &blob_id, GIT_FILEMODE_BLOB));
	}

	for (i = 0; i < nlinks; i++) {
		const char *slash = strrchr(links[i].path, '/');

		if (slash) {
			/* the tests only need a single directory level */
			git_treebuilder *sub = NULL;
			git_oid sub_id;
			git_str dirname = GIT_STR_INIT;

			cl_git_pass(git_str_set(
				&dirname, links[i].path, slash - links[i].path));
			cl_git_pass(git_treebuilder_new(&sub, repo, NULL));
			cl_git_pass(git_treebuilder_insert(
				NULL, sub, slash + 1, &links[i].target,
				GIT_FILEMODE_COMMIT));
			cl_git_pass(git_treebuilder_write(&sub_id, sub));
			git_treebuilder_free(sub);
			cl_git_pass(git_treebuilder_insert(
				NULL, root, dirname.ptr, &sub_id, GIT_FILEMODE_TREE));
			git_str_dispose(&dirname);
		} else {
			cl_git_pass(git_treebuilder_insert(
				NULL, root, links[i].path, &links[i].target,
				GIT_FILEMODE_COMMIT));
		}
	}

	cl_git_pass(git_treebuilder_write(&tree_id, root));
	git_treebuilder_free(root);
	cl_git_pass(git_tree_lookup(&tree, repo, &tree_id));

	if (parent_id) {
		cl_git_pass(git_commit_lookup(&parent, repo, parent_id));
		parents[0] = parent;
	}

	cl_git_pass(git_commit_create(
		out, repo, NULL, g_sig, g_sig, NULL, "super commit",
		tree, parent_id ? 1 : 0, parents));

	git_commit_free(parent);
	git_tree_free(tree);
}

static void reset_to(git_repository *repo, const git_oid *id)
{
	git_object *target;

	cl_git_pass(git_object_lookup(&target, repo, id, GIT_OBJECT_COMMIT));
	cl_git_pass(git_reset(repo, target, GIT_RESET_HARD, NULL));
	git_object_free(target);
}

/*
 * A fingerprint of everything the planner is not allowed to change:
 * HEAD, the workdir/index status and the set of objects in the odb.
 */
typedef struct {
	int head_valid;
	git_oid head;
	size_t status_entries;
	size_t odb_objects;
} repo_state;

static int count_odb_object(const git_oid *id, void *payload)
{
	GIT_UNUSED(id);
	(*(size_t *)payload)++;
	return 0;
}

static void capture_state(repo_state *st, git_repository *repo)
{
	git_status_options opts = GIT_STATUS_OPTIONS_INIT;
	git_status_list *list;
	git_odb *odb;

	memset(st, 0, sizeof(*st));

	if (git_reference_name_to_id(&st->head, repo, "HEAD") == 0)
		st->head_valid = 1;
	else
		git_error_clear();

	opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
		GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
	cl_git_pass(git_status_list_new(&list, repo, &opts));
	st->status_entries = git_status_list_entrycount(list);
	git_status_list_free(list);

	cl_git_pass(git_repository_odb(&odb, repo));
	cl_git_pass(git_odb_foreach(odb, count_odb_object, &st->odb_objects));
	git_odb_free(odb);
}

static void assert_state_unchanged(
	const repo_state *before, const repo_state *after, const char *what)
{
	cl_assert_equal_i(before->head_valid, after->head_valid);
	if (before->head_valid)
		cl_assert_equal_oid(&before->head, &after->head);
	cl_assert_equal_i(
		(int)before->status_entries, (int)after->status_entries);
	cl_assert_equal_i(
		(int)before->odb_objects, (int)after->odb_objects);

	fprintf(stderr,
		"[read-only evidence] %s unchanged: head_valid=%d "
		"status_entries=%zu odb_objects=%zu\n",
		what, after->head_valid,
		after->status_entries, after->odb_objects);
}

static const char *diag_type_name(git_submodule_plan_diagnostic_type_t type)
{
	switch (type) {
	case GIT_SUBMODULE_PLAN_DIAG_INVALID_PATH: return "INVALID_PATH";
	case GIT_SUBMODULE_PLAN_DIAG_DUPLICATE_NAME: return "DUPLICATE_NAME";
	case GIT_SUBMODULE_PLAN_DIAG_DUPLICATE_PATH: return "DUPLICATE_PATH";
	case GIT_SUBMODULE_PLAN_DIAG_GITLINK_WITHOUT_CONFIG:
		return "GITLINK_WITHOUT_CONFIG";
	case GIT_SUBMODULE_PLAN_DIAG_CONFIG_WITHOUT_GITLINK:
		return "CONFIG_WITHOUT_GITLINK";
	default: return "UNKNOWN";
	}
}

static void print_plan(const git_submodule_update_plan *plan)
{
	size_t i, n = git_submodule_update_plan_entry_count(plan);

	fprintf(stderr, "== submodule update plan: %zu entries ==\n", n);
	for (i = 0; i < n; i++) {
		const git_submodule_plan_entry *e =
			git_submodule_update_plan_entry_byindex(plan, i);
		char oldt[GIT_OID_MAX_HEXSIZE + 1];
		char newt[GIT_OID_MAX_HEXSIZE + 1];

		git_oid_tostr(oldt, sizeof(oldt), &e->old_target);
		git_oid_tostr(newt, sizeof(newt), &e->new_target);

		fprintf(stderr,
			"  entry '%s': flags=0x%02x old_path=%s new_path=%s\n"
			"    old_target=%s new_target=%s\n"
			"    initialized=%d head_matches_target=%d "
			"workdir_clean=%d target_object_exists=%d "
			"needs_fetch=%d\n",
			e->name, e->flags,
			e->old_path ? e->old_path : "(none)",
			e->new_path ? e->new_path : "(none)",
			oldt, newt,
			e->initialized, e->head_matches_target,
			e->workdir_clean, e->target_object_exists,
			e->needs_fetch);
	}

	n = git_submodule_update_plan_diagnostic_count(plan);
	fprintf(stderr, "== %zu diagnostics ==\n", n);
	for (i = 0; i < n; i++) {
		const git_submodule_plan_diagnostic *d =
			git_submodule_update_plan_diagnostic_byindex(plan, i);
		fprintf(stderr, "  diag[%d] %s: %s\n",
			(int)d->type, diag_type_name(d->type), d->message);
	}
}

/*
 * The core acceptance scenario: two superproject commits that differ
 * by one target OID upgrade and one path move.  The upgraded target
 * object is missing locally and must be flagged needs_fetch only;
 * the planner must not fetch or modify anything.
 */
void test_submodule_plan__upgrade_and_move(void)
{
	git_repository *src_a, *src_b, *super, *sub_clone;
	git_config *super_cfg;
	git_oid a1, a2, a3, b1, old_commit, new_commit;
	git_submodule_update_plan *plan;
	const git_submodule_plan_entry *e;
	repo_state super_before, super_after, sub_before, sub_after;
	test_gitlink old_links[2], new_links[2];
	clock_t start, end;

	src_a = create_repo("src_a");
	write_file_in_repo(src_a, "a.txt", "a1\n");
	commit_file(src_a, "a.txt", "a1", &a1);
	write_file_in_repo(src_a, "a.txt", "a2\n");
	commit_file(src_a, "a.txt", "a2", &a2);

	src_b = create_repo("src_b");
	write_file_in_repo(src_b, "b.txt", "b1\n");
	commit_file(src_b, "b.txt", "b1", &b1);

	super = create_repo("super");

	/* initialize sub_a locally: clone src_a and check out a1 */
	cl_git_pass(git_clone(&sub_clone, "src_a", "super/sub_a", NULL));
	reset_to(sub_clone, &a1);

	cl_git_pass(git_repository_config(&super_cfg, super));
	cl_git_pass(git_config_set_string(
		super_cfg, "submodule.sub_a.url", "../src_a"));
	git_config_free(super_cfg);

	/* a3 is created after the clone, so the clone lacks the object */
	write_file_in_repo(src_a, "a.txt", "a3\n");
	commit_file(src_a, "a.txt", "a3", &a3);

	memset(old_links, 0, sizeof(old_links));
	old_links[0].path = "sub_a";
	old_links[0].target = a1;
	old_links[1].path = "sub_b";
	old_links[1].target = b1;
	make_super_commit(super,
		"[submodule \"sub_a\"]\n\tpath = sub_a\n\turl = ../src_a\n"
		"[submodule \"sub_b\"]\n\tpath = sub_b\n\turl = ../src_b\n",
		old_links, 2, NULL, &old_commit);

	memset(new_links, 0, sizeof(new_links));
	new_links[0].path = "sub_a";
	new_links[0].target = a3;
	new_links[1].path = "modules/sub_b";
	new_links[1].target = b1;
	make_super_commit(super,
		"[submodule \"sub_a\"]\n\tpath = sub_a\n\turl = ../src_a\n"
		"[submodule \"sub_b\"]\n\tpath = modules/sub_b\n\turl = ../src_b\n",
		new_links, 2, &old_commit, &new_commit);

	capture_state(&super_before, super);
	capture_state(&sub_before, sub_clone);

	start = clock();
	cl_git_pass(git_submodule_update_plan_new(
		&plan, super, &old_commit, &new_commit));
	end = clock();

	cl_assert_equal_i(2, (int)git_submodule_update_plan_entry_count(plan));

	/* the OID upgrade is identified, and the missing target object
	 * is only flagged needs_fetch */
	e = git_submodule_update_plan_entry_byname(plan, "sub_a");
	cl_assert(e != NULL);
	cl_assert(e->flags & GIT_SUBMODULE_PLAN_ENTRY_TARGET_CHANGED);
	cl_assert(!(e->flags & GIT_SUBMODULE_PLAN_ENTRY_MOVED));
	cl_assert(!(e->flags & GIT_SUBMODULE_PLAN_ENTRY_ADDED));
	cl_assert(!(e->flags & GIT_SUBMODULE_PLAN_ENTRY_REMOVED));
	cl_assert_equal_s("sub_a", e->old_path);
	cl_assert_equal_s("sub_a", e->new_path);
	cl_assert_equal_oid(&a1, &e->old_target);
	cl_assert_equal_oid(&a3, &e->new_target);
	cl_assert_equal_i(1, e->initialized);
	cl_assert_equal_i(0, e->target_object_exists);
	cl_assert_equal_i(1, e->needs_fetch);
	cl_assert_equal_i(0, e->head_matches_target);
	cl_assert_equal_i(1, e->workdir_clean);

	/* the path move keeps the submodule identity (same name) */
	e = git_submodule_update_plan_entry_byname(plan, "sub_b");
	cl_assert(e != NULL);
	cl_assert(e->flags & GIT_SUBMODULE_PLAN_ENTRY_MOVED);
	cl_assert(!(e->flags & GIT_SUBMODULE_PLAN_ENTRY_TARGET_CHANGED));
	cl_assert(!(e->flags & GIT_SUBMODULE_PLAN_ENTRY_ADDED));
	cl_assert(!(e->flags & GIT_SUBMODULE_PLAN_ENTRY_REMOVED));
	cl_assert_equal_s("sub_b", e->old_path);
	cl_assert_equal_s("modules/sub_b", e->new_path);
	cl_assert_equal_oid(&b1, &e->old_target);
	cl_assert_equal_oid(&b1, &e->new_target);
	cl_assert_equal_i(0, e->initialized);
	cl_assert_equal_i(0, e->needs_fetch);

	cl_assert_equal_i(0, (int)git_submodule_update_plan_diagnostic_count(plan));

	print_plan(plan);
	fprintf(stderr, "[perf] plan computed in %.1f ms (budget: 90 s)\n",
		1000.0 * (double)(end - start) / CLOCKS_PER_SEC);

	git_submodule_update_plan_free(plan);

	/* prove that neither repository changed while planning */
	capture_state(&super_after, super);
	capture_state(&sub_after, sub_clone);
	assert_state_unchanged(&super_before, &super_after, "superproject");
	assert_state_unchanged(&sub_before, &sub_after, "sub_a");

	git_repository_free(sub_clone);
	git_repository_free(super);
	git_repository_free(src_a);
	git_repository_free(src_b);
}

/*
 * Added, removed and unchanged submodules, plus a submodule that was
 * deleted and recreated under the same name: the name keeps its
 * identity, so the plan must report a move plus config and target
 * changes, not an add/remove pair.
 */
void test_submodule_plan__add_remove_and_recreate_same_name(void)
{
	git_repository *src, *super;
	git_oid a1, b1, old_commit, new_commit;
	git_submodule_update_plan *plan;
	const git_submodule_plan_entry *e;
	test_gitlink old_links[3], new_links[3];

	src = create_repo("src");
	write_file_in_repo(src, "f.txt", "one\n");
	commit_file(src, "f.txt", "one", &a1);
	write_file_in_repo(src, "f.txt", "two\n");
	commit_file(src, "f.txt", "two", &b1);

	super = create_repo("super");

	memset(old_links, 0, sizeof(old_links));
	old_links[0].path = "keep";
	old_links[0].target = a1;
	old_links[1].path = "gone";
	old_links[1].target = a1;
	old_links[2].path = "old_loc";
	old_links[2].target = a1;
	make_super_commit(super,
		"[submodule \"sub_keep\"]\n\tpath = keep\n\turl = ../src\n"
		"[submodule \"sub_gone\"]\n\tpath = gone\n\turl = ../src\n"
		"[submodule \"sub\"]\n\tpath = old_loc\n\turl = ../old_url\n",
		old_links, 3, NULL, &old_commit);

	memset(new_links, 0, sizeof(new_links));
	new_links[0].path = "keep";
	new_links[0].target = a1;
	new_links[1].path = "fresh";
	new_links[1].target = a1;
	new_links[2].path = "new_loc";
	new_links[2].target = b1;
	make_super_commit(super,
		"[submodule \"sub_keep\"]\n\tpath = keep\n\turl = ../src\n"
		"[submodule \"sub_new\"]\n\tpath = fresh\n\turl = ../src\n"
		"[submodule \"sub\"]\n\tpath = new_loc\n\turl = ../new_url\n",
		new_links, 3, &old_commit, &new_commit);

	cl_git_pass(git_submodule_update_plan_new(
		&plan, super, &old_commit, &new_commit));

	cl_assert_equal_i(4, (int)git_submodule_update_plan_entry_count(plan));

	/* unchanged submodule: present in the plan, but with no flags */
	e = git_submodule_update_plan_entry_byname(plan, "sub_keep");
	cl_assert(e != NULL);
	cl_assert_equal_i(0, (int)e->flags);

	/* added submodule */
	e = git_submodule_update_plan_entry_byname(plan, "sub_new");
	cl_assert(e != NULL);
	cl_assert_equal_i(GIT_SUBMODULE_PLAN_ENTRY_ADDED, (int)e->flags);
	cl_assert(e->old_path == NULL);
	cl_assert_equal_s("fresh", e->new_path);
	cl_assert(git_oid_is_zero(&e->old_target));
	cl_assert_equal_oid(&a1, &e->new_target);

	/* removed submodule */
	e = git_submodule_update_plan_entry_byname(plan, "sub_gone");
	cl_assert(e != NULL);
	cl_assert_equal_i(GIT_SUBMODULE_PLAN_ENTRY_REMOVED, (int)e->flags);
	cl_assert_equal_s("gone", e->old_path);
	cl_assert(e->new_path == NULL);
	cl_assert_equal_oid(&a1, &e->old_target);
	cl_assert(git_oid_is_zero(&e->new_target));
	cl_assert_equal_i(0, e->initialized);
	cl_assert_equal_i(0, e->needs_fetch);

	/* deleted and recreated under the same name: one entry that
	 * keeps the identity and reports move + config + target change */
	e = git_submodule_update_plan_entry_byname(plan, "sub");
	cl_assert(e != NULL);
	cl_assert(e->flags & GIT_SUBMODULE_PLAN_ENTRY_MOVED);
	cl_assert(e->flags & GIT_SUBMODULE_PLAN_ENTRY_CONFIG_CHANGED);
	cl_assert(e->flags & GIT_SUBMODULE_PLAN_ENTRY_TARGET_CHANGED);
	cl_assert(!(e->flags & GIT_SUBMODULE_PLAN_ENTRY_ADDED));
	cl_assert(!(e->flags & GIT_SUBMODULE_PLAN_ENTRY_REMOVED));
	cl_assert_equal_s("old_loc", e->old_path);
	cl_assert_equal_s("new_loc", e->new_path);
	cl_assert_equal_oid(&a1, &e->old_target);
	cl_assert_equal_oid(&b1, &e->new_target);

	cl_assert_equal_i(0, (int)git_submodule_update_plan_diagnostic_count(plan));

	print_plan(plan);
	git_submodule_update_plan_free(plan);

	git_repository_free(super);
	git_repository_free(src);
}

/* NULL commits plan from/to the empty tree */
void test_submodule_plan__null_commits(void)
{
	git_repository *src, *super;
	git_oid a1, commit;
	git_submodule_update_plan *plan;
	const git_submodule_plan_entry *e;
	test_gitlink links[1];

	src = create_repo("src");
	write_file_in_repo(src, "f.txt", "one\n");
	commit_file(src, "f.txt", "one", &a1);

	super = create_repo("super");

	memset(links, 0, sizeof(links));
	links[0].path = "sub";
	links[0].target = a1;
	make_super_commit(super,
		"[submodule \"sub\"]\n\tpath = sub\n\turl = ../src\n",
		links, 1, NULL, &commit);

	cl_git_pass(git_submodule_update_plan_new(&plan, super, NULL, &commit));
	cl_assert_equal_i(1, (int)git_submodule_update_plan_entry_count(plan));
	e = git_submodule_update_plan_entry_byname(plan, "sub");
	cl_assert(e != NULL);
	cl_assert_equal_i(GIT_SUBMODULE_PLAN_ENTRY_ADDED, (int)e->flags);
	git_submodule_update_plan_free(plan);

	cl_git_pass(git_submodule_update_plan_new(&plan, super, &commit, NULL));
	cl_assert_equal_i(1, (int)git_submodule_update_plan_entry_count(plan));
	e = git_submodule_update_plan_entry_byname(plan, "sub");
	cl_assert(e != NULL);
	cl_assert_equal_i(GIT_SUBMODULE_PLAN_ENTRY_REMOVED, (int)e->flags);
	git_submodule_update_plan_free(plan);

	git_repository_free(super);
	git_repository_free(src);
}

/*
 * A dirty initialized submodule is reported as not clean; an
 * uninitialized submodule is reported as not initialized with all
 * local inspection fields unknown, and no fetch is requested.
 */
void test_submodule_plan__dirty_and_uninitialized(void)
{
	git_repository *src, *super, *sub_clone;
	git_config *super_cfg;
	git_oid a1, old_commit, new_commit;
	git_submodule_update_plan *plan;
	const git_submodule_plan_entry *e;
	repo_state super_before, super_after, sub_before, sub_after;
	test_gitlink links[2];

	src = create_repo("src");
	write_file_in_repo(src, "f.txt", "one\n");
	commit_file(src, "f.txt", "one", &a1);

	super = create_repo("super");

	/* initialize sub_dirty locally; its HEAD matches the target */
	cl_git_pass(git_clone(&sub_clone, "src", "super/sub_dirty", NULL));

	cl_git_pass(git_repository_config(&super_cfg, super));
	cl_git_pass(git_config_set_string(
		super_cfg, "submodule.sub_dirty.url", "../src"));
	git_config_free(super_cfg);

	/* make the submodule workdir dirty */
	write_file_in_repo(sub_clone, "dirty.txt", "dirty\n");

	memset(links, 0, sizeof(links));
	links[0].path = "sub_dirty";
	links[0].target = a1;
	links[1].path = "sub_uninit";
	links[1].target = a1;
	make_super_commit(super,
		"[submodule \"sub_dirty\"]\n\tpath = sub_dirty\n\turl = ../src\n"
		"[submodule \"sub_uninit\"]\n\tpath = sub_uninit\n\turl = ../src\n",
		links, 2, NULL, &old_commit);
	make_super_commit(super,
		"[submodule \"sub_dirty\"]\n\tpath = sub_dirty\n\turl = ../src\n"
		"[submodule \"sub_uninit\"]\n\tpath = sub_uninit\n\turl = ../src\n",
		links, 2, &old_commit, &new_commit);

	capture_state(&super_before, super);
	capture_state(&sub_before, sub_clone);

	cl_git_pass(git_submodule_update_plan_new(
		&plan, super, &old_commit, &new_commit));

	cl_assert_equal_i(2, (int)git_submodule_update_plan_entry_count(plan));

	e = git_submodule_update_plan_entry_byname(plan, "sub_dirty");
	cl_assert(e != NULL);
	cl_assert_equal_i(0, (int)e->flags);
	cl_assert_equal_i(1, e->initialized);
	cl_assert_equal_i(0, e->workdir_clean);
	cl_assert_equal_i(1, e->head_matches_target);
	cl_assert_equal_i(1, e->target_object_exists);
	cl_assert_equal_i(0, e->needs_fetch);

	e = git_submodule_update_plan_entry_byname(plan, "sub_uninit");
	cl_assert(e != NULL);
	cl_assert_equal_i(0, (int)e->flags);
	cl_assert_equal_i(0, e->initialized);
	cl_assert_equal_i(-1, e->workdir_clean);
	cl_assert_equal_i(-1, e->head_matches_target);
	cl_assert_equal_i(-1, e->target_object_exists);
	cl_assert_equal_i(0, e->needs_fetch);

	print_plan(plan);
	git_submodule_update_plan_free(plan);

	capture_state(&super_after, super);
	capture_state(&sub_after, sub_clone);
	assert_state_unchanged(&super_before, &super_after, "superproject");
	assert_state_unchanged(&sub_before, &sub_after, "sub_dirty");

	git_repository_free(sub_clone);
	git_repository_free(super);
	git_repository_free(src);
}

/*
 * Malicious and inconsistent submodule data must be diagnosed, never
 * acted upon: parent-directory traversal, absolute paths, duplicate
 * names, duplicate paths, gitlinks without configuration and
 * configuration without gitlinks.
 */
void test_submodule_plan__malicious_and_inconsistent(void)
{
	git_repository *src, *super;
	git_oid a1, old_commit, new_commit;
	git_submodule_update_plan *plan;
	const git_submodule_plan_entry *e;
	const git_submodule_plan_diagnostic *d;
	repo_state super_before, super_after;
	test_gitlink links[3];
	unsigned int seen = 0;
	size_t i, n;

	src = create_repo("src");
	write_file_in_repo(src, "f.txt", "one\n");
	commit_file(src, "f.txt", "one", &a1);

	super = create_repo("super");
	make_super_commit(super, NULL, NULL, 0, NULL, &old_commit);

	memset(links, 0, sizeof(links));
	links[0].path = "ghost";
	links[0].target = a1;
	links[1].path = "shared";
	links[1].target = a1;
	links[2].path = "d2";
	links[2].target = a1;
	make_super_commit(super,
		"[submodule \"evil\"]\n\tpath = ../evil\n\turl = ../src\n"
		"[submodule \"abs\"]\n\tpath = /etc/abs\n\turl = ../src\n"
		"[submodule \"dup\"]\n\tpath = d1\n\turl = ../src\n"
		"[submodule \"dup\"]\n\tpath = d2\n\turl = ../src\n"
		"[submodule \"samepath1\"]\n\tpath = shared\n\turl = ../src\n"
		"[submodule \"samepath2\"]\n\tpath = shared\n\turl = ../src\n"
		"[submodule \"nogitlink\"]\n\tpath = no_link\n\turl = ../src\n",
		links, 3, &old_commit, &new_commit);

	capture_state(&super_before, super);

	cl_git_pass(git_submodule_update_plan_new(
		&plan, super, &old_commit, &new_commit));

	/* every configured submodule is planned as added */
	cl_assert_equal_i(6, (int)git_submodule_update_plan_entry_count(plan));

	/* invalid paths are never inspected on disk */
	e = git_submodule_update_plan_entry_byname(plan, "evil");
	cl_assert(e != NULL);
	cl_assert_equal_s("../evil", e->new_path);
	cl_assert_equal_i(0, e->initialized);
	cl_assert_equal_i(0, e->needs_fetch);
	cl_assert_equal_i(-1, e->workdir_clean);

	e = git_submodule_update_plan_entry_byname(plan, "abs");
	cl_assert(e != NULL);
	cl_assert_equal_i(0, e->initialized);
	cl_assert_equal_i(0, e->needs_fetch);

	/* the duplicate name keeps the last configured path */
	e = git_submodule_update_plan_entry_byname(plan, "dup");
	cl_assert(e != NULL);
	cl_assert_equal_s("d2", e->new_path);
	cl_assert_equal_oid(&a1, &e->new_target);

	n = git_submodule_update_plan_diagnostic_count(plan);
	cl_assert_equal_i(6, (int)n);

	for (i = 0; i < n; i++) {
		d = git_submodule_update_plan_diagnostic_byindex(plan, i);
		cl_assert(d->message != NULL);
		seen |= (1u << (unsigned int)d->type);
	}

	cl_assert(seen & (1u << GIT_SUBMODULE_PLAN_DIAG_INVALID_PATH));
	cl_assert(seen & (1u << GIT_SUBMODULE_PLAN_DIAG_DUPLICATE_NAME));
	cl_assert(seen & (1u << GIT_SUBMODULE_PLAN_DIAG_DUPLICATE_PATH));
	cl_assert(seen & (1u << GIT_SUBMODULE_PLAN_DIAG_GITLINK_WITHOUT_CONFIG));
	cl_assert(seen & (1u << GIT_SUBMODULE_PLAN_DIAG_CONFIG_WITHOUT_GITLINK));

	/* the two invalid paths were both diagnosed */
	{
		unsigned int invalid_paths = 0;
		for (i = 0; i < n; i++) {
			d = git_submodule_update_plan_diagnostic_byindex(plan, i);
			if (d->type == GIT_SUBMODULE_PLAN_DIAG_INVALID_PATH)
				invalid_paths++;
		}
		cl_assert_equal_i(2, (int)invalid_paths);
	}

	print_plan(plan);
	git_submodule_update_plan_free(plan);

	/* nothing escaped the sandbox and nothing changed */
	cl_assert(!git_fs_path_isdir("evil"));
	cl_assert(!git_fs_path_isdir("../evil"));
	capture_state(&super_after, super);
	assert_state_unchanged(&super_before, &super_after, "superproject");

	git_repository_free(super);
	git_repository_free(src);
}

/* out-of-range accessors and empty plans behave sanely */
void test_submodule_plan__accessors(void)
{
	git_repository *super;
	git_oid empty_commit;
	git_submodule_update_plan *plan;

	super = create_repo("super");
	make_super_commit(super, NULL, NULL, 0, NULL, &empty_commit);

	cl_git_pass(git_submodule_update_plan_new(
		&plan, super, &empty_commit, &empty_commit));
	cl_assert_equal_i(0, (int)git_submodule_update_plan_entry_count(plan));
	cl_assert_equal_i(0, (int)git_submodule_update_plan_diagnostic_count(plan));
	cl_assert(git_submodule_update_plan_entry_byindex(plan, 0) == NULL);
	cl_assert(git_submodule_update_plan_entry_byindex(plan, 42) == NULL);
	cl_assert(git_submodule_update_plan_entry_byname(plan, "nope") == NULL);
	cl_assert(git_submodule_update_plan_diagnostic_byindex(plan, 0) == NULL);

	git_submodule_update_plan_free(plan);
	git_submodule_update_plan_free(NULL); /* no-op */

	git_repository_free(super);
}
