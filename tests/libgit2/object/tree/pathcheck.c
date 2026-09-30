#include "clar_libgit2.h"

#include "git2/tree.h"
#include "git2/blob.h"

static git_repository *g_repo;

void test_object_tree_pathcheck__initialize(void)
{
	g_repo = cl_git_sandbox_init("empty_standard_repo");
}

void test_object_tree_pathcheck__cleanup(void)
{
	cl_git_sandbox_cleanup();
}

static void create_blob(git_oid *oid, const char *name)
{
	cl_git_pass(git_blob_create_from_buffer(
		oid, g_repo, name, strlen(name)));
}

static void insert_blob(git_treebuilder *bld, const char *name, git_oid *blob)
{
	cl_git_pass(git_treebuilder_insert(
		NULL, bld, name, blob, GIT_FILEMODE_BLOB));
}

static void write_tree(git_oid *oid, git_treebuilder *bld)
{
	cl_git_pass(git_treebuilder_write(oid, bld));
	git_treebuilder_free(bld);
}

static void assert_problem(
	const git_tree_path_report *report,
	size_t idx,
	const char *path,
	git_tree_path_problem_t problem)
{
	const git_tree_path_problem *entry;

	entry = git_tree_path_report_byindex(report, idx);
	cl_assert(entry != NULL);
	cl_assert_equal_s(path, entry->path);
	cl_assert_equal_i(problem, entry->problem);
}

void test_object_tree_pathcheck__simple_case_and_device_names(void)
{
	git_oid blob_a, blob_b, blob_con, blob_ok, tree_id;
	git_treebuilder *bld;
	git_tree *tree;
	git_tree_path_report *report;

	create_blob(&blob_a, "A contents");
	create_blob(&blob_b, "a contents");
	create_blob(&blob_con, "con contents");
	create_blob(&blob_ok, "ok contents");

	cl_git_pass(git_treebuilder_new(&bld, g_repo, NULL));
	insert_blob(bld, "A.txt", &blob_a);
	insert_blob(bld, "a.txt", &blob_b);
	insert_blob(bld, "con", &blob_con);
	insert_blob(bld, "normal.txt", &blob_ok);
	write_tree(&tree_id, bld);

	cl_git_pass(git_tree_lookup(&tree, g_repo, &tree_id));

	cl_git_pass(git_tree_path_report_new(&report, tree,
		GIT_TREE_PATH_PLATFORM_SENSITIVE));
	cl_assert_equal_i(0, git_tree_path_report_entrycount(report));
	cl_assert(git_tree_path_report_byindex(report, 0) == NULL);
	git_tree_path_report_free(report);

	cl_git_pass(git_tree_path_report_new(&report, tree,
		GIT_TREE_PATH_PLATFORM_WINDOWS));
	cl_assert_equal_i(3, git_tree_path_report_entrycount(report));
	assert_problem(report, 0, "A.txt",
		GIT_TREE_PATH_PROBLEM_CASE_COLLISION);
	assert_problem(report, 1, "a.txt",
		GIT_TREE_PATH_PROBLEM_CASE_COLLISION);
	assert_problem(report, 2, "con",
		GIT_TREE_PATH_PROBLEM_RESERVED_NAME);
	git_tree_path_report_free(report);

	git_tree_free(tree);
}

void test_object_tree_pathcheck__nested_trailing_and_directory_merge(void)
{
	git_oid blob;
	git_oid dir_upper_id, dir_lower_id, sub_id;
	git_oid tree_id, gitlink_id;
	git_treebuilder *bld;
	git_tree *tree;
	git_tree_path_report *sensitive, *windows;

	create_blob(&blob, "blob");
	git_oid_from_string(&gitlink_id,
		"deadbeefdeadbeefdeadbeefdeadbeefdeadbeef", GIT_OID_SHA1);

	/* Dir/F and dir/f merge after ASCII case folding and collide. */
	cl_git_pass(git_treebuilder_new(&bld, g_repo, NULL));
	insert_blob(bld, "F", &blob);
	write_tree(&dir_upper_id, bld);

	cl_git_pass(git_treebuilder_new(&bld, g_repo, NULL));
	insert_blob(bld, "f", &blob);
	write_tree(&dir_lower_id, bld);

	cl_git_pass(git_treebuilder_new(&bld, g_repo, NULL));
	insert_blob(bld, "trailing_dot.", &blob);
	insert_blob(bld, "nested.txt", &blob);
	write_tree(&sub_id, bld);

	cl_git_pass(git_treebuilder_new(&bld, g_repo, NULL));

	/* Directories whose names merge after case folding. */
	cl_git_pass(git_treebuilder_insert(NULL, bld, "Dir",
		&dir_upper_id, GIT_FILEMODE_TREE));
	cl_git_pass(git_treebuilder_insert(NULL, bld, "dir",
		&dir_lower_id, GIT_FILEMODE_TREE));

	/* Trailing dots and spaces, nested and at the root. */
	cl_git_pass(git_treebuilder_insert(NULL, bld, "sub",
		&sub_id, GIT_FILEMODE_TREE));
	insert_blob(bld, "trail.", &blob);
	insert_blob(bld, "space ", &blob);

	/* A submodule (gitlink) and ordinary Unicode names. */
	cl_git_pass(git_treebuilder_insert(NULL, bld, "submodule",
		&gitlink_id, GIT_FILEMODE_COMMIT));
	insert_blob(bld, "\xc3\xa9.txt", &blob); /* U+00E9 in UTF-8 */
	insert_blob(bld, "normal.txt", &blob);

	write_tree(&tree_id, bld);

	cl_git_pass(git_tree_lookup(&tree, g_repo, &tree_id));

	cl_git_pass(git_tree_path_report_new(&sensitive, tree,
		GIT_TREE_PATH_PLATFORM_SENSITIVE));
	cl_assert_equal_i(0, git_tree_path_report_entrycount(sensitive));

	cl_git_pass(git_tree_path_report_new(&windows, tree,
		GIT_TREE_PATH_PLATFORM_WINDOWS));

	git_tree_free(tree);

	cl_assert_equal_i(5, git_tree_path_report_entrycount(windows));
	assert_problem(windows, 0, "Dir/F",
		GIT_TREE_PATH_PROBLEM_CASE_COLLISION);
	assert_problem(windows, 1, "dir/f",
		GIT_TREE_PATH_PROBLEM_CASE_COLLISION);
	assert_problem(windows, 2, "space ",
		GIT_TREE_PATH_PROBLEM_TRAILING_SPACE);
	assert_problem(windows, 3, "sub/trailing_dot.",
		GIT_TREE_PATH_PROBLEM_TRAILING_DOT);
	assert_problem(windows, 4, "trail.",
		GIT_TREE_PATH_PROBLEM_TRAILING_DOT);

	git_tree_path_report_free(sensitive);
	git_tree_path_report_free(windows);
}

void test_object_tree_pathcheck__file_directory_collision(void)
{
	git_oid blob, dir_id, tree_id;
	git_treebuilder *bld;
	git_tree *tree;
	git_tree_path_report *report;

	create_blob(&blob, "blob");

	cl_git_pass(git_treebuilder_new(&bld, g_repo, NULL));
	insert_blob(bld, "inner", &blob);
	write_tree(&dir_id, bld);

	cl_git_pass(git_treebuilder_new(&bld, g_repo, NULL));
	insert_blob(bld, "readme", &blob);
	cl_git_pass(git_treebuilder_insert(NULL, bld, "Readme",
		&dir_id, GIT_FILEMODE_TREE));
	write_tree(&tree_id, bld);

	cl_git_pass(git_tree_lookup(&tree, g_repo, &tree_id));
	cl_git_pass(git_tree_path_report_new(&report, tree,
		GIT_TREE_PATH_PLATFORM_WINDOWS));

	cl_assert_equal_i(2, git_tree_path_report_entrycount(report));
	assert_problem(report, 0, "Readme",
		GIT_TREE_PATH_PROBLEM_FILE_DIRECTORY_COLLISION);
	assert_problem(report, 1, "readme",
		GIT_TREE_PATH_PROBLEM_FILE_DIRECTORY_COLLISION);

	git_tree_path_report_free(report);
	git_tree_free(tree);
}

void test_object_tree_pathcheck__invalid_arguments(void)
{
	git_oid blob, tree_id;
	git_treebuilder *bld;
	git_tree *tree;
	git_tree_path_report *report;

	create_blob(&blob, "x");
	cl_git_pass(git_treebuilder_new(&bld, g_repo, NULL));
	insert_blob(bld, "x", &blob);
	write_tree(&tree_id, bld);
	cl_git_pass(git_tree_lookup(&tree, g_repo, &tree_id));

	cl_git_fail_with(GIT_EINVALIDSPEC, git_tree_path_report_new(
		&report, tree, (git_tree_path_platform_t)42));
	cl_git_pass(git_tree_path_report_new(&report, tree,
		GIT_TREE_PATH_PLATFORM_WINDOWS));
	cl_assert_equal_i(0, git_tree_path_report_entrycount(report));
	git_tree_path_report_free(report);
	git_tree_path_report_free(NULL);

	git_tree_free(tree);
}

void test_object_tree_pathcheck__windows_name_variants(void)
{
	git_oid blob, tree_id;
	git_treebuilder *bld;
	git_tree *tree;
	git_tree_path_report *report;

	create_blob(&blob, "x");

	cl_git_pass(git_treebuilder_new(&bld, g_repo, NULL));
	insert_blob(bld, "Com1", &blob);
	insert_blob(bld, "lpt9", &blob);
	insert_blob(bld, "lpt0", &blob);
	insert_blob(bld, "NUL.", &blob);
	insert_blob(bld, "normal", &blob);
	write_tree(&tree_id, bld);

	cl_git_pass(git_tree_lookup(&tree, g_repo, &tree_id));
	cl_git_pass(git_tree_path_report_new(&report, tree,
		GIT_TREE_PATH_PLATFORM_WINDOWS));

	cl_assert_equal_i(4, git_tree_path_report_entrycount(report));
	assert_problem(report, 0, "Com1",
		GIT_TREE_PATH_PROBLEM_RESERVED_NAME);
	assert_problem(report, 1, "NUL.",
		GIT_TREE_PATH_PROBLEM_RESERVED_NAME);
	assert_problem(report, 2, "NUL.",
		GIT_TREE_PATH_PROBLEM_TRAILING_DOT);
	assert_problem(report, 3, "lpt9",
		GIT_TREE_PATH_PROBLEM_RESERVED_NAME);

	git_tree_path_report_free(report);
	git_tree_free(tree);
}

void test_object_tree_pathcheck__non_ascii_is_not_folded(void)
{
	git_oid blob, tree_id;
	git_treebuilder *bld;
	git_tree *tree;
	git_tree_path_report *report;

	create_blob(&blob, "x");

	cl_git_pass(git_treebuilder_new(&bld, g_repo, NULL));
	/* U+00C9 (\xc3\x89) vs U+00E9 (\xc3\xa9): no ASCII folding. */
	insert_blob(bld, "\xc3\x89.txt", &blob);
	insert_blob(bld, "\xc3\xa9.txt", &blob);
	write_tree(&tree_id, bld);

	cl_git_pass(git_tree_lookup(&tree, g_repo, &tree_id));
	cl_git_pass(git_tree_path_report_new(&report, tree,
		GIT_TREE_PATH_PLATFORM_WINDOWS));
	cl_assert_equal_i(0, git_tree_path_report_entrycount(report));
	git_tree_path_report_free(report);
	git_tree_free(tree);
}
