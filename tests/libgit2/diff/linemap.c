#include "clar_libgit2.h"

static git_repository *g_repo;

void test_diff_linemap__initialize(void)
{
	g_repo = cl_git_sandbox_init("empty_standard_repo");
}

void test_diff_linemap__cleanup(void)
{
	cl_git_sandbox_cleanup();
}

/*
 * Build a diff between two versions of "file.txt": the old content is
 * staged in the index, the new content is written to the workdir.
 */
static git_diff *make_text_diff(const char *old_content, const char *new_content,
	uint32_t context_lines)
{
	git_index *index;
	git_diff *diff = NULL;
	git_diff_options opts = GIT_DIFF_OPTIONS_INIT;

	opts.context_lines = context_lines;

	cl_git_rewritefile("empty_standard_repo/file.txt", old_content);

	cl_git_pass(git_repository_index(&index, g_repo));
	cl_git_pass(git_index_add_bypath(index, "file.txt"));
	cl_git_pass(git_index_write(index));
	git_index_free(index);

	cl_git_rewritefile("empty_standard_repo/file.txt", new_content);

	cl_git_pass(git_diff_index_to_workdir(&diff, g_repo, NULL, &opts));

	return diff;
}

static void write_workdir_file(const char *name, const void *data, size_t len)
{
	char fullpath[2048];
	FILE *fp;
	size_t written;

	snprintf(fullpath, sizeof(fullpath), "%s/%s",
		git_repository_workdir(g_repo), name);

	cl_assert(fp = fopen(fullpath, "wb"));
	written = fwrite(data, 1, len, fp);
	fclose(fp);

	cl_assert(written == len);
}

static git_diff *make_binary_diff(void)
{
	static const char old_data[] = { 'a', '\0', 'b', '\0', 1, 2, 3, 4 };
	static const char new_data[] = { 'a', '\0', 'b', '\0', 5, 6, 7, 8, 9 };
	git_index *index;
	git_diff *diff = NULL;

	write_workdir_file("file.bin", old_data, sizeof(old_data));

	cl_git_pass(git_repository_index(&index, g_repo));
	cl_git_pass(git_index_add_bypath(index, "file.bin"));
	cl_git_pass(git_index_write(index));
	git_index_free(index);

	write_workdir_file("file.bin", new_data, sizeof(new_data));

	cl_git_pass(git_diff_index_to_workdir(&diff, g_repo, NULL, NULL));

	return diff;
}

static void assert_old_to_new(const git_diff_linemap *map, int old_lineno, int new_lineno)
{
	git_diff_linemap_result result;

	cl_git_pass(git_diff_linemap_old_to_new(&result, map, old_lineno));
	cl_assert_equal_i(GIT_DIFF_LINEMAP_OK, result.status);
	cl_assert_equal_i(new_lineno, result.lineno);
}

static void assert_new_to_old(const git_diff_linemap *map, int new_lineno, int old_lineno)
{
	git_diff_linemap_result result;

	cl_git_pass(git_diff_linemap_new_to_old(&result, map, new_lineno));
	cl_assert_equal_i(GIT_DIFF_LINEMAP_OK, result.status);
	cl_assert_equal_i(old_lineno, result.lineno);
}

void test_diff_linemap__unchanged_lines_map_exactly(void)
{
	git_diff *diff = make_text_diff("A\nB\nC\n", "A\nX\nB\nC\n", 3);
	git_diff_linemap *map;

	cl_git_pass(git_diff_linemap_new(&map, diff, 0));

	/* old A/B/C -> new A/X/B/C: B moves from old line 2 to new line 3 */
	assert_old_to_new(map, 1, 1);
	assert_old_to_new(map, 2, 3);
	assert_old_to_new(map, 3, 4);

	git_diff_linemap_free(map);
	git_diff_free(diff);
}

void test_diff_linemap__added_lines_have_a_reverse_query(void)
{
	git_diff *diff = make_text_diff("A\nB\nC\n", "A\nX\nB\nC\n", 3);
	git_diff_linemap *map;
	git_diff_linemap_result result;

	cl_git_pass(git_diff_linemap_new(&map, diff, 0));

	/* the added line X reports GIT_DIFF_LINEMAP_ADDED */
	cl_git_pass(git_diff_linemap_new_to_old(&result, map, 2));
	cl_assert_equal_i(GIT_DIFF_LINEMAP_ADDED, result.status);
	cl_assert_equal_i(-1, result.lineno);
	cl_assert_equal_sz(0, result.hunk_idx);
	/* X was inserted before old line 2 (B) */
	cl_assert_equal_i(2, result.adjacent_lineno);

	/* unchanged lines map back exactly */
	assert_new_to_old(map, 1, 1);
	assert_new_to_old(map, 3, 2);
	assert_new_to_old(map, 4, 3);

	git_diff_linemap_free(map);
	git_diff_free(diff);
}

void test_diff_linemap__deleted_lines_report_deletion(void)
{
	git_diff *diff = make_text_diff("A\nB\nC\n", "A\nC\n", 3);
	git_diff_linemap *map;
	git_diff_linemap_result result;

	cl_git_pass(git_diff_linemap_new(&map, diff, 0));

	/*
	 * B was deleted; querying it must report the deletion (with the
	 * adjacent hunk position), not map it onto the neighboring C.
	 */
	cl_git_pass(git_diff_linemap_old_to_new(&result, map, 2));
	cl_assert_equal_i(GIT_DIFF_LINEMAP_DELETED, result.status);
	cl_assert_equal_i(-1, result.lineno);
	cl_assert_equal_sz(0, result.hunk_idx);
	/* new line 2 (C) now sits where B used to be */
	cl_assert_equal_i(2, result.adjacent_lineno);

	assert_old_to_new(map, 1, 1);
	assert_old_to_new(map, 3, 2);

	git_diff_linemap_free(map);
	git_diff_free(diff);
}

void test_diff_linemap__index_outlives_diff(void)
{
	git_diff *diff = make_text_diff("A\nB\nC\n", "A\nX\nB\nC\n", 3);
	git_diff_linemap *map;

	cl_git_pass(git_diff_linemap_new(&map, diff, 0));

	/* release the diff first; the index keeps its own copies */
	git_diff_free(diff);

	assert_old_to_new(map, 2, 3);
	assert_new_to_old(map, 3, 2);

	git_diff_linemap_free(map);
}

void test_diff_linemap__diff_outlives_index(void)
{
	git_diff *diff = make_text_diff("A\nB\nC\n", "A\nX\nB\nC\n", 3);
	git_diff_linemap *map;

	cl_git_pass(git_diff_linemap_new(&map, diff, 0));

	/* releasing the index must not touch the diff */
	git_diff_linemap_free(map);

	cl_assert_equal_sz(1, git_diff_num_deltas(diff));

	/* and a second index can still be built from the same diff */
	cl_git_pass(git_diff_linemap_new(&map, diff, 0));
	assert_old_to_new(map, 2, 3);

	git_diff_linemap_free(map);
	git_diff_free(diff);
}

void test_diff_linemap__zero_context_hunks(void)
{
	git_diff *diff = make_text_diff("A\nB\nC\n", "A\nX\nB\nC\n", 0);
	git_diff_linemap *map;
	git_diff_linemap_result result;

	cl_git_pass(git_diff_linemap_new(&map, diff, 0));

	/*
	 * with zero context lines the hunk is a pure addition
	 * (@@ -1,0 +2 @@); all old lines map through the hunk offsets
	 */
	assert_old_to_new(map, 1, 1);
	assert_old_to_new(map, 2, 3);
	assert_old_to_new(map, 3, 4);

	cl_git_pass(git_diff_linemap_new_to_old(&result, map, 2));
	cl_assert_equal_i(GIT_DIFF_LINEMAP_ADDED, result.status);
	cl_assert_equal_sz(0, result.hunk_idx);
	/* X was inserted after old line 1 (ie before old line 2) */
	cl_assert_equal_i(2, result.adjacent_lineno);

	assert_new_to_old(map, 1, 1);
	assert_new_to_old(map, 3, 2);
	assert_new_to_old(map, 4, 3);

	git_diff_linemap_free(map);
	git_diff_free(diff);
}

void test_diff_linemap__no_newline_at_eof(void)
{
	/* the last line of both files has no trailing newline */
	git_diff *diff = make_text_diff("A\nB\nC", "A\nB\nD", 3);
	git_diff_linemap *map;
	git_diff_linemap_result result;

	cl_git_pass(git_diff_linemap_new(&map, diff, 0));

	assert_old_to_new(map, 1, 1);
	assert_old_to_new(map, 2, 2);

	/* old line 3 ("C") was replaced; it is reported as deleted */
	cl_git_pass(git_diff_linemap_old_to_new(&result, map, 3));
	cl_assert_equal_i(GIT_DIFF_LINEMAP_DELETED, result.status);
	cl_assert_equal_i(3, result.adjacent_lineno);

	/* new line 3 ("D") is an addition */
	cl_git_pass(git_diff_linemap_new_to_old(&result, map, 3));
	cl_assert_equal_i(GIT_DIFF_LINEMAP_ADDED, result.status);
	cl_assert_equal_i(4, result.adjacent_lineno);

	assert_new_to_old(map, 1, 1);
	assert_new_to_old(map, 2, 2);

	git_diff_linemap_free(map);
	git_diff_free(diff);
}

void test_diff_linemap__unchanged_lines_between_hunks(void)
{
	git_diff *diff = make_text_diff(
		"l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\nl10\n",
		"l1\nx2a\nx2b\nl3\nl4\nl5\nl6\nl7\nl8\nl10\n",
		1);
	git_diff_linemap *map;

	cl_git_pass(git_diff_linemap_new(&map, diff, 0));

	/*
	 * two hunks: l2 replaced by two lines (+1), l9 deleted (-1);
	 * lines between the hunks pick up the cumulative offset
	 */
	assert_old_to_new(map, 4, 5);
	assert_old_to_new(map, 5, 6);
	assert_old_to_new(map, 7, 8);
	assert_old_to_new(map, 10, 10);

	assert_new_to_old(map, 5, 4);
	assert_new_to_old(map, 8, 7);
	assert_new_to_old(map, 9, 8);
	assert_new_to_old(map, 10, 10);

	git_diff_linemap_free(map);
	git_diff_free(diff);
}

void test_diff_linemap__binary_files_are_unmappable(void)
{
	git_diff *diff = make_binary_diff();
	git_diff_linemap *map;
	git_diff_linemap_result result;

	cl_assert_equal_sz(1, git_diff_num_deltas(diff));

	cl_git_pass(git_diff_linemap_new(&map, diff, 0));

	cl_git_pass(git_diff_linemap_old_to_new(&result, map, 1));
	cl_assert_equal_i(GIT_DIFF_LINEMAP_UNMAPPABLE, result.status);
	cl_assert_equal_i(-1, result.lineno);

	cl_git_pass(git_diff_linemap_new_to_old(&result, map, 1));
	cl_assert_equal_i(GIT_DIFF_LINEMAP_UNMAPPABLE, result.status);
	cl_assert_equal_i(-1, result.lineno);

	git_diff_linemap_free(map);
	git_diff_free(diff);
}

void test_diff_linemap__delta_index_out_of_range(void)
{
	git_diff *diff = make_text_diff("A\n", "B\n", 3);
	git_diff_linemap *map = NULL;

	cl_git_fail_with(GIT_ENOTFOUND, git_diff_linemap_new(&map, diff, 1));
	cl_assert(map == NULL);

	git_diff_free(diff);
}
