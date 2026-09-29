#include "clar_libgit2.h"

#include <git2.h>

#include "futils.h"

/*
 * The test fixture builds the following commit graph in a fresh
 * repository, including a merge and a common ancestor:
 *
 *     M (merge, parents: B and C)
 *     B (parent: A)   C (parent: A)
 *     A (root, no parents)
 */

static git_repository *g_repo;
static git_oid g_root;    /* A */
static git_oid g_branch1; /* B */
static git_oid g_branch2; /* C */
static git_oid g_merge;   /* M */

static void make_commit(git_oid *out, const char *message,
	const git_oid *parents, size_t parent_count)
{
	git_signature *sig;
	git_treebuilder *builder;
	git_tree *tree;
	git_oid tree_oid;
	git_commit *parent_commits[2];
	const git_commit *parent_ptrs[2];
	size_t i;

	cl_assert(parent_count <= 2);

	cl_git_pass(git_signature_new(&sig, "Traverse Test", "traverse@example.com", 1234567890, 0));

	cl_git_pass(git_treebuilder_new(&builder, g_repo, NULL));
	cl_git_pass(git_treebuilder_write(&tree_oid, builder));
	git_treebuilder_free(builder);
	cl_git_pass(git_tree_lookup(&tree, g_repo, &tree_oid));

	for (i = 0; i < parent_count; i++) {
		cl_git_pass(git_commit_lookup(&parent_commits[i], g_repo, &parents[i]));
		parent_ptrs[i] = parent_commits[i];
	}

	cl_git_pass(git_commit_create(out, g_repo, NULL, sig, sig, NULL,
		message, tree, parent_count, parent_ptrs));

	for (i = 0; i < parent_count; i++)
		git_commit_free(parent_commits[i]);

	git_tree_free(tree);
	git_signature_free(sig);
}

void test_graph_traverse__initialize(void)
{
	git_reference *ref;
	git_oid parents[2];

	cl_git_pass(git_repository_init(&g_repo, "./traverse", 0));

	make_commit(&g_root, "root", NULL, 0);

	parents[0] = g_root;
	make_commit(&g_branch1, "branch 1", parents, 1);
	make_commit(&g_branch2, "branch 2", parents, 1);

	parents[0] = g_branch1;
	parents[1] = g_branch2;
	make_commit(&g_merge, "merge", parents, 2);

	cl_git_pass(git_reference_create(&ref, g_repo, "refs/heads/main", &g_merge, 0, NULL));
	git_reference_free(ref);
}

void test_graph_traverse__cleanup(void)
{
	git_repository_free(g_repo);
	g_repo = NULL;

	cl_fixture_cleanup("./traverse");
}

static int report_visited_contains(const git_graph_report *report, const git_oid *oid)
{
	size_t i;

	for (i = 0; i < report->visited_count; i++) {
		if (git_oid_equal(&report->visited[i], oid))
			return 1;
	}

	return 0;
}

static int report_has_edge(const git_graph_report *report,
	const git_oid *from, const git_oid *to, git_graph_boundary_reason reason)
{
	size_t i;

	for (i = 0; i < report->edge_count; i++) {
		const git_graph_boundary_edge *edge = &report->edges[i];

		if (git_oid_equal(&edge->from, from) &&
		    git_oid_equal(&edge->to, to) &&
		    edge->reason == reason)
			return 1;
	}

	return 0;
}

static void reopen_repo(void)
{
	git_repository_free(g_repo);
	g_repo = NULL;

	cl_git_pass(git_repository_open(&g_repo, "./traverse"));
}

static void loose_object_path(git_str *out, const git_oid *oid)
{
	char hex[GIT_OID_SHA1_HEXSIZE + 1];

	git_oid_tostr(hex, sizeof(hex), oid);

	cl_git_pass(git_str_printf(out, "%sobjects/%.2s/%s",
		git_repository_path(g_repo), hex, hex + 2));
}

void test_graph_traverse__full_history(void)
{
	git_graph_report *report;
	git_oid starts[1];

	starts[0] = g_merge;

	cl_git_pass(git_graph_traverse(&report, g_repo, starts, 1, NULL, 0));

	cl_assert_equal_i(4, report->visited_count);
	cl_assert(report_visited_contains(report, &g_merge));
	cl_assert(report_visited_contains(report, &g_branch1));
	cl_assert(report_visited_contains(report, &g_branch2));
	cl_assert(report_visited_contains(report, &g_root));
	cl_assert_equal_i(0, report->edge_count);

	git_graph_report_free(report);
}

void test_graph_traverse__stop_at_common_ancestor(void)
{
	git_graph_report *report;
	git_oid starts[1], stops[1];

	starts[0] = g_merge;
	stops[0] = g_root;

	cl_git_pass(git_graph_traverse(&report, g_repo, starts, 1, stops, 1));

	cl_assert_equal_i(3, report->visited_count);
	cl_assert(report_visited_contains(report, &g_merge));
	cl_assert(report_visited_contains(report, &g_branch1));
	cl_assert(report_visited_contains(report, &g_branch2));
	cl_assert(!report_visited_contains(report, &g_root));

	cl_assert_equal_i(2, report->edge_count);
	cl_assert(report_has_edge(report, &g_branch1, &g_root, GIT_GRAPH_BOUNDARY_STOP));
	cl_assert(report_has_edge(report, &g_branch2, &g_root, GIT_GRAPH_BOUNDARY_STOP));

	git_graph_report_free(report);
}

void test_graph_traverse__stop_at_branch_point(void)
{
	git_graph_report *report;
	git_oid starts[1], stops[1];

	starts[0] = g_merge;
	stops[0] = g_branch1;

	cl_git_pass(git_graph_traverse(&report, g_repo, starts, 1, stops, 1));

	cl_assert_equal_i(3, report->visited_count);
	cl_assert(report_visited_contains(report, &g_merge));
	cl_assert(report_visited_contains(report, &g_branch2));
	cl_assert(report_visited_contains(report, &g_root));
	cl_assert(!report_visited_contains(report, &g_branch1));

	cl_assert_equal_i(1, report->edge_count);
	cl_assert(report_has_edge(report, &g_merge, &g_branch1, GIT_GRAPH_BOUNDARY_STOP));

	git_graph_report_free(report);
}

void test_graph_traverse__multiple_starts(void)
{
	git_graph_report *report;
	git_oid starts[2];

	starts[0] = g_branch1;
	starts[1] = g_branch2;

	cl_git_pass(git_graph_traverse(&report, g_repo, starts, 2, NULL, 0));

	cl_assert_equal_i(3, report->visited_count);
	cl_assert(report_visited_contains(report, &g_branch1));
	cl_assert(report_visited_contains(report, &g_branch2));
	cl_assert(report_visited_contains(report, &g_root));
	cl_assert_equal_i(0, report->edge_count);

	git_graph_report_free(report);

	/* overlapping starts are only visited once */
	starts[0] = g_merge;
	starts[1] = g_branch1;

	cl_git_pass(git_graph_traverse(&report, g_repo, starts, 2, NULL, 0));

	cl_assert_equal_i(4, report->visited_count);
	cl_assert_equal_i(0, report->edge_count);

	git_graph_report_free(report);
}

void test_graph_traverse__start_is_stop(void)
{
	git_graph_report *report;
	git_oid starts[1], stops[1];

	starts[0] = g_merge;
	stops[0] = g_merge;

	cl_git_pass(git_graph_traverse(&report, g_repo, starts, 1, stops, 1));

	cl_assert_equal_i(0, report->visited_count);
	cl_assert_equal_i(1, report->edge_count);
	cl_assert(report_has_edge(report, &g_merge, &g_merge, GIT_GRAPH_BOUNDARY_STOP));

	git_graph_report_free(report);
}

void test_graph_traverse__root_commit(void)
{
	git_graph_report *report;
	git_oid starts[1];

	starts[0] = g_root;

	cl_git_pass(git_graph_traverse(&report, g_repo, starts, 1, NULL, 0));

	cl_assert_equal_i(1, report->visited_count);
	cl_assert(report_visited_contains(report, &g_root));
	cl_assert_equal_i(0, report->edge_count);

	git_graph_report_free(report);
}

void test_graph_traverse__missing_parent(void)
{
	git_graph_report *report;
	git_oid starts[1];
	git_str path = GIT_STR_INIT;

	loose_object_path(&path, &g_root);
	cl_git_pass(p_chmod(git_str_cstr(&path), 0777));
	cl_git_pass(p_unlink(git_str_cstr(&path)));
	git_str_dispose(&path);

	reopen_repo();

	starts[0] = g_merge;

	cl_git_pass(git_graph_traverse(&report, g_repo, starts, 1, NULL, 0));

	cl_assert_equal_i(3, report->visited_count);
	cl_assert(report_visited_contains(report, &g_merge));
	cl_assert(report_visited_contains(report, &g_branch1));
	cl_assert(report_visited_contains(report, &g_branch2));
	cl_assert(!report_visited_contains(report, &g_root));

	cl_assert_equal_i(2, report->edge_count);
	cl_assert(report_has_edge(report, &g_branch1, &g_root, GIT_GRAPH_BOUNDARY_MISSING));
	cl_assert(report_has_edge(report, &g_branch2, &g_root, GIT_GRAPH_BOUNDARY_MISSING));

	git_graph_report_free(report);
}

void test_graph_traverse__corrupt_parent(void)
{
	git_graph_report *report;
	git_oid starts[1];
	git_str path = GIT_STR_INIT;
	git_str garbage = GIT_STR_INIT;

	loose_object_path(&path, &g_root);
	cl_git_pass(p_chmod(git_str_cstr(&path), 0777));
	cl_git_pass(git_str_puts(&garbage, "this is not a valid zlib stream"));
	cl_git_pass(git_futils_writebuffer(&garbage, git_str_cstr(&path),
		O_WRONLY | O_CREAT | O_TRUNC, 0644));
	git_str_dispose(&garbage);
	git_str_dispose(&path);

	reopen_repo();

	starts[0] = g_merge;

	cl_git_pass(git_graph_traverse(&report, g_repo, starts, 1, NULL, 0));

	cl_assert_equal_i(3, report->visited_count);
	cl_assert(!report_visited_contains(report, &g_root));

	cl_assert_equal_i(2, report->edge_count);
	cl_assert(report_has_edge(report, &g_branch1, &g_root, GIT_GRAPH_BOUNDARY_ERROR));
	cl_assert(report_has_edge(report, &g_branch2, &g_root, GIT_GRAPH_BOUNDARY_ERROR));

	git_graph_report_free(report);
}

void test_graph_traverse__invalid_start_oid(void)
{
	git_graph_report *report = NULL;
	git_oid starts[1];

	cl_git_pass(git_oid_from_string(&starts[0],
		"1234567890123456789012345678901234567890", GIT_OID_SHA1));

	cl_git_fail_with(GIT_ENOTFOUND,
		git_graph_traverse(&report, g_repo, starts, 1, NULL, 0));
	cl_assert(report == NULL);
}

void test_graph_traverse__does_not_modify_repo(void)
{
	git_graph_report *report;
	git_oid starts[1];
	git_strarray refs_before, refs_after;
	git_reference *head;
	git_status_list *status;
	git_index *index;
	const char *head_target;
	size_t i;

	cl_git_pass(git_reference_list(&refs_before, g_repo));

	cl_git_pass(git_reference_lookup(&head, g_repo, "HEAD"));
	cl_assert(git_reference_type(head) == GIT_REFERENCE_SYMBOLIC);
	head_target = git__strdup(git_reference_symbolic_target(head));
	git_reference_free(head);

	starts[0] = g_merge;
	cl_git_pass(git_graph_traverse(&report, g_repo, starts, 1, NULL, 0));
	cl_assert_equal_i(4, report->visited_count);
	git_graph_report_free(report);

	/* references are unchanged */
	cl_git_pass(git_reference_list(&refs_after, g_repo));
	cl_assert_equal_i(refs_before.count, refs_after.count);
	for (i = 0; i < refs_before.count; i++)
		cl_assert_equal_s(refs_before.strings[i], refs_after.strings[i]);

	cl_git_pass(git_reference_lookup(&head, g_repo, "refs/heads/main"));
	cl_assert_equal_oid(&g_merge, git_reference_target(head));
	git_reference_free(head);

	cl_git_pass(git_reference_lookup(&head, g_repo, "HEAD"));
	cl_assert_equal_s(head_target, git_reference_symbolic_target(head));
	git_reference_free(head);

	/* the index is untouched */
	cl_git_pass(git_repository_index(&index, g_repo));
	cl_assert_equal_i(0, git_index_entrycount(index));
	git_index_free(index);

	/* the working directory is clean */
	cl_git_pass(git_status_list_new(&status, g_repo, NULL));
	cl_assert_equal_i(0, git_status_list_entrycount(status));
	git_status_list_free(status);

	git__free((char *)head_target);
	git_strarray_dispose(&refs_before);
	git_strarray_dispose(&refs_after);
}
