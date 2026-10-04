#include "clar_libgit2.h"
#include "git2/lineindex.h"

static git_repository *g_repo = NULL;

void test_diff_lineindex__initialize(void)
{
	g_repo = cl_git_sandbox_init("empty_bare.git");
}

void test_diff_lineindex__cleanup(void)
{
	cl_git_sandbox_cleanup();
}

static git_tree *make_tree(const char *path, const char *content)
{
	git_oid blob_id, tree_id;
	git_treebuilder *bld;
	git_tree *tree = NULL;

	cl_git_pass(git_blob_create_from_buffer(&blob_id, g_repo,
		content, strlen(content)));
	cl_git_pass(git_treebuilder_new(&bld, g_repo, NULL));
	cl_git_pass(git_treebuilder_insert(NULL, bld, path,
		&blob_id, GIT_FILEMODE_BLOB));
	cl_git_pass(git_treebuilder_write(&tree_id, bld));
	cl_git_pass(git_tree_lookup(&tree, g_repo, &tree_id));
	git_treebuilder_free(bld);
	return tree;
}

static git_tree *make_tree_bin(const char *path, const void *data, size_t len)
{
	git_oid blob_id, tree_id;
	git_treebuilder *bld;
	git_tree *tree = NULL;

	cl_git_pass(git_blob_create_from_buffer(&blob_id, g_repo, data, len));
	cl_git_pass(git_treebuilder_new(&bld, g_repo, NULL));
	cl_git_pass(git_treebuilder_insert(NULL, bld, path,
		&blob_id, GIT_FILEMODE_BLOB));
	cl_git_pass(git_treebuilder_write(&tree_id, bld));
	cl_git_pass(git_tree_lookup(&tree, g_repo, &tree_id));
	git_treebuilder_free(bld);
	return tree;
}

/*
 * old: A / B / C
 * new: A / X / B / C
 *
 * The inserted X shifts B from old line 2 to new line 3.
 */
void test_diff_lineindex__insertion_maps_shift(void)
{
	git_tree *old_tree = make_tree("file.txt", "A\nB\nC\n");
	git_tree *new_tree = make_tree("file.txt", "A\nX\nB\nC\n");
	git_diff *diff = NULL;
	git_lineindex *idx = NULL;
	git_lineindex_location loc;

	cl_git_pass(git_diff_tree_to_tree(&diff, g_repo, old_tree, new_tree, NULL));
	cl_git_pass(git_lineindex_new(&idx, diff));

	cl_assert_equal_sz(1, git_lineindex_delta_count(idx));

	/* old line 2 (B) -> new line 3 */
	cl_git_pass(git_lineindex_lookup_old(idx, 0, 2, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_CONTEXT, loc.status);
	cl_assert_equal_i(2, loc.old_lineno);
	cl_assert_equal_i(3, loc.new_lineno);

	/* old line 1 (A) -> new line 1 */
	cl_git_pass(git_lineindex_lookup_old(idx, 0, 1, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_CONTEXT, loc.status);
	cl_assert_equal_i(1, loc.old_lineno);
	cl_assert_equal_i(1, loc.new_lineno);

	/* old line 3 (C) -> new line 4 */
	cl_git_pass(git_lineindex_lookup_old(idx, 0, 3, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_CONTEXT, loc.status);
	cl_assert_equal_i(3, loc.old_lineno);
	cl_assert_equal_i(4, loc.new_lineno);

	/* reverse: new line 2 (X) is added */
	cl_git_pass(git_lineindex_lookup_new(idx, 0, 2, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_ADDED, loc.status);
	cl_assert_equal_i(2, loc.new_lineno);
	cl_assert_equal_i(0, loc.old_lineno);
	cl_assert_equal_i(1, loc.before);
	cl_assert_equal_i(2, loc.after);

	/* reverse: new line 3 (B) -> old line 2 */
	cl_git_pass(git_lineindex_lookup_new(idx, 0, 3, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_CONTEXT, loc.status);
	cl_assert_equal_i(3, loc.new_lineno);
	cl_assert_equal_i(2, loc.old_lineno);

	git_lineindex_free(idx);
	git_diff_free(diff);
	git_tree_free(new_tree);
	git_tree_free(old_tree);
}

/*
 * old: A / B / C
 * new: A / C
 *
 * B is deleted.  Querying old line 2 must report DELETED, not map it to C.
 */
void test_diff_lineindex__deletion_returns_deleted(void)
{
	git_tree *old_tree = make_tree("file.txt", "A\nB\nC\n");
	git_tree *new_tree = make_tree("file.txt", "A\nC\n");
	git_diff *diff = NULL;
	git_lineindex *idx = NULL;
	git_lineindex_location loc;

	cl_git_pass(git_diff_tree_to_tree(&diff, g_repo, old_tree, new_tree, NULL));
	cl_git_pass(git_lineindex_new(&idx, diff));

	cl_git_pass(git_lineindex_lookup_old(idx, 0, 2, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_DELETED, loc.status);
	cl_assert_equal_i(2, loc.old_lineno);
	cl_assert_equal_i(0, loc.new_lineno);
	/* deletion is between new line 1 (A) and new line 2 (C) */
	cl_assert_equal_i(1, loc.before);
	cl_assert_equal_i(2, loc.after);

	/* old line 1 (A) -> new line 1 */
	cl_git_pass(git_lineindex_lookup_old(idx, 0, 1, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_CONTEXT, loc.status);
	cl_assert_equal_i(1, loc.new_lineno);

	/* old line 3 (C) -> new line 2 */
	cl_git_pass(git_lineindex_lookup_old(idx, 0, 3, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_CONTEXT, loc.status);
	cl_assert_equal_i(3, loc.old_lineno);
	cl_assert_equal_i(2, loc.new_lineno);

	/* reverse: new line 2 (C) -> old line 3 */
	cl_git_pass(git_lineindex_lookup_new(idx, 0, 2, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_CONTEXT, loc.status);
	cl_assert_equal_i(2, loc.new_lineno);
	cl_assert_equal_i(3, loc.old_lineno);

	git_lineindex_free(idx);
	git_diff_free(diff);
	git_tree_free(new_tree);
	git_tree_free(old_tree);
}

/*
 * Zero-context hunks: the hunk contains only the changed line.
 */
void test_diff_lineindex__zero_context(void)
{
	git_tree *old_tree = make_tree("file.txt", "A\nB\nC\nD\nE\n");
	git_tree *new_tree = make_tree("file.txt", "A\nB\nX\nC\nD\nE\n");
	git_diff *diff = NULL;
	git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
	git_lineindex *idx = NULL;
	git_lineindex_location loc;

	opts.context_lines = 0;
	cl_git_pass(git_diff_tree_to_tree(&diff, g_repo, old_tree, new_tree, &opts));
	cl_git_pass(git_lineindex_new(&idx, diff));


	/* old line 3 (C) -> new line 4 (X inserted before C) */
	cl_git_pass(git_lineindex_lookup_old(idx, 0, 3, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_CONTEXT, loc.status);
	cl_assert_equal_i(3, loc.old_lineno);
	cl_assert_equal_i(4, loc.new_lineno);

	/* old line 2 (B) -> new line 2 */
	cl_git_pass(git_lineindex_lookup_old(idx, 0, 2, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_CONTEXT, loc.status);
	cl_assert_equal_i(2, loc.new_lineno);

	/* reverse: new line 3 (X) is added */
	cl_git_pass(git_lineindex_lookup_new(idx, 0, 3, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_ADDED, loc.status);
	cl_assert_equal_i(3, loc.new_lineno);
	cl_assert_equal_i(2, loc.before);
	cl_assert_equal_i(3, loc.after);

	git_lineindex_free(idx);
	git_diff_free(diff);
	git_tree_free(new_tree);
	git_tree_free(old_tree);
}

/*
 * No newline at end of file.
 */
void test_diff_lineindex__no_newline_at_eof(void)
{
	git_tree *old_tree = make_tree("file.txt", "A\nB\nC");
	git_tree *new_tree = make_tree("file.txt", "A\nX\nB\nC");
	git_diff *diff = NULL;
	git_lineindex *idx = NULL;
	git_lineindex_location loc;

	cl_git_pass(git_diff_tree_to_tree(&diff, g_repo, old_tree, new_tree, NULL));
	cl_git_pass(git_lineindex_new(&idx, diff));

	cl_git_pass(git_lineindex_lookup_old(idx, 0, 2, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_CONTEXT, loc.status);
	cl_assert_equal_i(2, loc.old_lineno);
	cl_assert_equal_i(3, loc.new_lineno);

	cl_git_pass(git_lineindex_lookup_old(idx, 0, 3, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_CONTEXT, loc.status);
	cl_assert_equal_i(3, loc.old_lineno);
	cl_assert_equal_i(4, loc.new_lineno);

	cl_git_pass(git_lineindex_lookup_new(idx, 0, 4, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_CONTEXT, loc.status);
	cl_assert_equal_i(4, loc.new_lineno);
	cl_assert_equal_i(3, loc.old_lineno);

	git_lineindex_free(idx);
	git_diff_free(diff);
	git_tree_free(new_tree);
	git_tree_free(old_tree);
}

/*
 * Binary files are explicitly unmappable.
 */
void test_diff_lineindex__binary_unmappable(void)
{
	static const unsigned char old_data[] = "A\nB\nC\n\x00\n";
	static const unsigned char new_data[] = "A\nB\nC\nD\n\x00\n";
	git_tree *old_tree = make_tree_bin("file.bin", old_data, sizeof(old_data) - 1);
	git_tree *new_tree = make_tree_bin("file.bin", new_data, sizeof(new_data) - 1);
	git_diff *diff = NULL;
	git_lineindex *idx = NULL;
	git_lineindex_location loc;
	git_lineindex_delta_info info;

	cl_git_pass(git_diff_tree_to_tree(&diff, g_repo, old_tree, new_tree, NULL));
	cl_git_pass(git_lineindex_new(&idx, diff));

	cl_git_pass(git_lineindex_get_delta_info(idx, 0, &info));
	cl_assert(info.binary);

	cl_git_pass(git_lineindex_lookup_old(idx, 0, 2, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_UNMAPPABLE, loc.status);

	cl_git_pass(git_lineindex_lookup_new(idx, 0, 3, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_UNMAPPABLE, loc.status);

	git_lineindex_free(idx);
	git_diff_free(diff);
	git_tree_free(new_tree);
	git_tree_free(old_tree);
}

/*
 * The index is independent of the diff: freeing the diff first does not
 * invalidate the index.
 */
void test_diff_lineindex__free_diff_before_index(void)
{
	git_tree *old_tree = make_tree("file.txt", "A\nB\nC\n");
	git_tree *new_tree = make_tree("file.txt", "A\nX\nB\nC\n");
	git_diff *diff = NULL;
	git_lineindex *idx = NULL;
	git_lineindex_location loc;

	cl_git_pass(git_diff_tree_to_tree(&diff, g_repo, old_tree, new_tree, NULL));
	cl_git_pass(git_lineindex_new(&idx, diff));

	/* free the diff first */
	git_diff_free(diff);

	/* the index must still work */
	cl_git_pass(git_lineindex_lookup_old(idx, 0, 2, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_CONTEXT, loc.status);
	cl_assert_equal_i(3, loc.new_lineno);

	git_lineindex_free(idx);
	git_tree_free(new_tree);
	git_tree_free(old_tree);
}

/*
 * The index is independent of the diff: freeing the index first also
 * leaves the diff usable (and vice versa).
 */
void test_diff_lineindex__free_index_before_diff(void)
{
	git_tree *old_tree = make_tree("file.txt", "A\nB\nC\n");
	git_tree *new_tree = make_tree("file.txt", "A\nX\nB\nC\n");
	git_diff *diff = NULL;
	git_lineindex *idx = NULL;
	git_lineindex_location loc;

	cl_git_pass(git_diff_tree_to_tree(&diff, g_repo, old_tree, new_tree, NULL));
	cl_git_pass(git_lineindex_new(&idx, diff));

	/* free the index first */
	git_lineindex_free(idx);

	/* the diff must still be usable */
	cl_assert_equal_sz(1, git_diff_num_deltas(diff));

	git_diff_free(diff);
	git_tree_free(new_tree);
	git_tree_free(old_tree);
}

/*
 * Out-of-range line numbers and absent sides are reported as NONE.
 */
void test_diff_lineindex__out_of_range(void)
{
	git_tree *old_tree = make_tree("file.txt", "A\nB\nC\n");
	git_tree *new_tree = make_tree("file.txt", "A\nX\nB\nC\n");
	git_diff *diff = NULL;
	git_lineindex *idx = NULL;
	git_lineindex_location loc;

	cl_git_pass(git_diff_tree_to_tree(&diff, g_repo, old_tree, new_tree, NULL));
	cl_git_pass(git_lineindex_new(&idx, diff));

	cl_git_pass(git_lineindex_lookup_old(idx, 0, 0, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_NONE, loc.status);

	cl_git_pass(git_lineindex_lookup_old(idx, 0, 99, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_NONE, loc.status);

	cl_git_pass(git_lineindex_lookup_new(idx, 0, 99, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_NONE, loc.status);

	cl_assert_equal_i(GIT_ENOTFOUND,
		git_lineindex_lookup_old(idx, 1, 1, &loc));

	git_lineindex_free(idx);
	git_diff_free(diff);
	git_tree_free(new_tree);
	git_tree_free(old_tree);
}

/*
 * Multiple hunks: unchanged lines between hunks map exactly.
 */
void test_diff_lineindex__multiple_hunks(void)
{
	git_tree *old_tree = make_tree("file.txt",
		"a1\na2\na3\na4\na5\na6\na7\na8\na9\n"
		"b1\nb2\nb3\nb4\nb5\nb6\nb7\nb8\nb9\n"
		"c1\nc2\nc3\nc4\nc5\n");
	git_tree *new_tree = make_tree("file.txt",
		"a1\na2\na3\na4\na5\na6\na7\na8\na9\n"
		"B1\nb2\nb3\nb4\nb5\nb6\nb7\nb8\nb9\n"
		"c1\nc2\nc3\nc4\nc5\n");
	git_diff *diff = NULL;
	git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
	git_lineindex *idx = NULL;
	git_lineindex_location loc;

	opts.context_lines = 1;
	cl_git_pass(git_diff_tree_to_tree(&diff, g_repo, old_tree, new_tree, &opts));
	cl_git_pass(git_lineindex_new(&idx, diff));

	/* line 10 (b1) -> deleted */
	cl_git_pass(git_lineindex_lookup_old(idx, 0, 10, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_DELETED, loc.status);

	/* line 11 (b2) -> new line 11 */
	cl_git_pass(git_lineindex_lookup_old(idx, 0, 11, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_CONTEXT, loc.status);
	cl_assert_equal_i(11, loc.new_lineno);

	/* line 1 (a1) -> new line 1 */
	cl_git_pass(git_lineindex_lookup_old(idx, 0, 1, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_CONTEXT, loc.status);
	cl_assert_equal_i(1, loc.new_lineno);

	/* line 23 (c5) -> new line 23 */
	cl_git_pass(git_lineindex_lookup_old(idx, 0, 23, &loc));
	cl_assert_equal_i(GIT_LINEINDEX_CONTEXT, loc.status);
	cl_assert_equal_i(23, loc.new_lineno);

	git_lineindex_free(idx);
	git_diff_free(diff);
	git_tree_free(new_tree);
	git_tree_free(old_tree);
}
