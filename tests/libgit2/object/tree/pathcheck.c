#include "clar_libgit2.h"

#include "git2/tree.h"
#include "git2/sys/mempack.h"

static git_repository *repo;
static git_odb *odb;
static git_oid blob_oid, gitlink_oid;

static void insert_leaf(
	git_treebuilder *bld,
	const char *name,
	git_filemode_t mode)
{
	const git_oid *id = (mode == GIT_FILEMODE_COMMIT) ?
		&gitlink_oid : &blob_oid;

	/* Keep the first mode if the same raw name was inserted twice. */
	if (git_treebuilder_get(bld, name) == NULL)
		cl_git_pass(git_treebuilder_insert(
			NULL, bld, name, id, mode));
}

static void insert_path(
	git_treebuilder *bld,
	const char *path,
	git_filemode_t mode)
{
	const char *slash = strchr(path, '/');

	if (slash == NULL) {
		insert_leaf(bld, path, mode);
		return;
	}

	{
		size_t dir_len = (size_t)(slash - path);
		char *dirname = git__malloc(dir_len + 1);
		git_treebuilder *sub_bld;
		git_tree *sub_tree = NULL;
		const git_tree_entry *existing;
		git_oid sub_oid;

		cl_assert(dirname != NULL);
		memcpy(dirname, path, dir_len);
		dirname[dir_len] = '\0';

		existing = git_treebuilder_get(bld, dirname);
		if (existing != NULL) {
			cl_assert(git_tree_entry_filemode(existing) ==
				GIT_FILEMODE_TREE);
			cl_git_pass(git_tree_lookup(&sub_tree, repo,
				git_tree_entry_id(existing)));
		}

		cl_git_pass(git_treebuilder_new(&sub_bld, repo, sub_tree));
		git_tree_free(sub_tree);
		insert_path(sub_bld, slash + 1, mode);
		cl_git_pass(git_treebuilder_write(&sub_oid, sub_bld));
		git_treebuilder_free(sub_bld);

		cl_git_pass(git_treebuilder_insert(NULL, bld, dirname,
			&sub_oid, GIT_FILEMODE_TREE));
		git__free(dirname);
	}
}

typedef struct {
	const char *path;
	git_filemode_t mode;
} test_path;

typedef struct {
	const char *path;
	uint32_t issues;
} expected_issue;

static git_tree *build_tree(const test_path *paths, size_t npaths)
{
	git_treebuilder *bld;
	git_oid tree_oid;
	git_tree *tree;
	size_t i;

	tree = NULL;

	for (i = 0; i < npaths; i++) {
		cl_git_pass(git_treebuilder_new(&bld, repo, tree));
		git_tree_free(tree);
		tree = NULL;

		insert_path(bld, paths[i].path, paths[i].mode);
		cl_git_pass(git_treebuilder_write(&tree_oid, bld));
		git_treebuilder_free(bld);

		if (i + 1 < npaths) {
			cl_git_pass(git_tree_lookup(&tree, repo, &tree_oid));
		}
	}

	cl_git_pass(git_tree_lookup(&tree, repo, &tree_oid));
	return tree;
}

static void assert_check(
	const git_tree *tree,
	git_tree_path_policy_t policy,
	const expected_issue *expected,
	size_t nexpected)
{
	git_tree_path_issue_list *list;
	size_t i;

	cl_git_pass(git_tree_check_path_issues(&list, tree, policy));
	cl_assert_equal_sz(nexpected, list->len);

	for (i = 0; i < nexpected; i++) {
		cl_assert_equal_s(expected[i].path, list->items[i].path);
		cl_assert_equal_i(expected[i].issues,
			(int)list->items[i].issues);
	}

	git_tree_path_issue_list_free(list);
}

#define T(path) { path, GIT_FILEMODE_BLOB }
#define X(path) { path, GIT_FILEMODE_BLOB_EXECUTABLE }
#define S(path) { path, GIT_FILEMODE_COMMIT }
#define P(path, issues) { path, issues }

void test_object_tree_pathcheck__initialize(void)
{
	git_odb_backend *backend;

	cl_git_pass(git_mempack_new(&backend));
	cl_git_pass(git_odb_new(&odb));
	cl_git_pass(git_odb_add_backend(odb, backend, 10));
	cl_git_pass(git_repository_wrap_odb(&repo, odb));

	cl_git_pass(git_blob_create_from_buffer(&blob_oid, repo,
		"hello", 5));
	cl_git_pass(git_oid_from_string(&gitlink_oid,
		"0123456789012345678901234567890123456789", GIT_OID_SHA1));
}

void test_object_tree_pathcheck__cleanup(void)
{
	git_repository_free(repo);
	git_odb_free(odb);
}

void test_object_tree_pathcheck__case_sensitive_allows_everything(void)
{
	const test_path paths[] = {
		T("A.txt"),
		T("a.txt"),
		T("con"),
		T("normal.txt")
	};
	git_tree *tree = build_tree(paths, 4);

	assert_check(tree, GIT_TREE_PATH_POLICY_CASE_SENSITIVE, NULL, 0);

	git_tree_free(tree);
}

void test_object_tree_pathcheck__windows_case_and_device_names(void)
{
	const test_path paths[] = {
		T("A.txt"),
		T("a.txt"),
		T("con"),
		T("normal.txt")
	};
	const expected_issue expected[] = {
		P("A.txt", GIT_TREE_PATH_ISSUE_CASE_COLLISION),
		P("a.txt", GIT_TREE_PATH_ISSUE_CASE_COLLISION),
		P("con", GIT_TREE_PATH_ISSUE_RESERVED_NAME)
	};
	git_tree *tree = build_tree(paths, 4);

	assert_check(tree, GIT_TREE_PATH_POLICY_WINDOWS, expected, 3);

	git_tree_free(tree);
}

void test_object_tree_pathcheck__windows_nested_directories(void)
{
	const test_path paths[] = {
		T("src/foo/file.txt"),
		T("src/bar/file.txt"),
		X("bin/tool")
	};
	git_tree *tree = build_tree(paths, 3);

	assert_check(tree, GIT_TREE_PATH_POLICY_CASE_SENSITIVE, NULL, 0);
	assert_check(tree, GIT_TREE_PATH_POLICY_WINDOWS, NULL, 0);

	git_tree_free(tree);
}

void test_object_tree_pathcheck__windows_trailing_dot_and_space(void)
{
	const test_path paths[] = {
		T("ok/file.txt"),
		T("bad./file.txt"),
		T("dir/spaces /inside.txt"),
		T("end.dot."),
		T("endspace ")
	};
	const expected_issue expected[] = {
		P("bad.", GIT_TREE_PATH_ISSUE_TRAILING_DOT),
		P("bad./file.txt", GIT_TREE_PATH_ISSUE_TRAILING_DOT),
		P("dir/spaces ", GIT_TREE_PATH_ISSUE_TRAILING_SPACE),
		P("dir/spaces /inside.txt",
			GIT_TREE_PATH_ISSUE_TRAILING_SPACE),
		P("end.dot.", GIT_TREE_PATH_ISSUE_TRAILING_DOT),
		P("endspace ", GIT_TREE_PATH_ISSUE_TRAILING_SPACE)
	};
	git_tree *tree = build_tree(paths, 5);

	assert_check(tree, GIT_TREE_PATH_POLICY_WINDOWS, expected, 6);
	assert_check(tree, GIT_TREE_PATH_POLICY_CASE_SENSITIVE, NULL, 0);

	git_tree_free(tree);
}

void test_object_tree_pathcheck__windows_collision_inside_merged_dirs(void)
{
	/* "SRC" and "src" merge into one directory on Windows, and the
	 * inner "file.txt" entries collide after that merge. */
	const test_path paths[] = {
		T("SRC/file.txt"),
		T("src/FILE.txt"),
		T("src/other.txt"),
		T("SRC/only.txt")
	};
	const expected_issue expected[] = {
		P("SRC/file.txt", GIT_TREE_PATH_ISSUE_CASE_COLLISION),
		P("src/FILE.txt", GIT_TREE_PATH_ISSUE_CASE_COLLISION)
	};
	git_tree *tree = build_tree(paths, 4);

	assert_check(tree, GIT_TREE_PATH_POLICY_WINDOWS, expected, 2);
	assert_check(tree, GIT_TREE_PATH_POLICY_CASE_SENSITIVE, NULL, 0);

	git_tree_free(tree);
}

void test_object_tree_pathcheck__windows_clean_dir_merge(void)
{
	/* Merged directories with disjoint contents check out fine. */
	const test_path paths[] = {
		T("Docs/a.txt"),
		T("DOCS/b.txt")
	};
	git_tree *tree = build_tree(paths, 2);

	assert_check(tree, GIT_TREE_PATH_POLICY_WINDOWS, NULL, 0);

	git_tree_free(tree);
}

void test_object_tree_pathcheck__windows_dir_and_file_collision(void)
{
	const test_path paths[] = {
		T("foo/bar.txt"),
		T("FOO"),
		T("con/log.txt")
	};
	const expected_issue expected[] = {
		P("FOO", GIT_TREE_PATH_ISSUE_CASE_COLLISION),
		P("con", GIT_TREE_PATH_ISSUE_RESERVED_NAME),
		P("con/log.txt", GIT_TREE_PATH_ISSUE_RESERVED_NAME),
		P("foo", GIT_TREE_PATH_ISSUE_CASE_COLLISION)
	};
	git_tree *tree = build_tree(paths, 3);

	assert_check(tree, GIT_TREE_PATH_POLICY_WINDOWS, expected, 4);

	git_tree_free(tree);
}

void test_object_tree_pathcheck__submodule_and_unicode_names(void)
{
	const test_path paths[] = {
		S("sub"),
		S("vendor/dep"),
		T("\xC3\xA9\xC3\xA8.txt"),
		T("\xC3\x89.txt"),
		T("normal.txt")
	};
	git_tree *tree = build_tree(paths, 5);

	assert_check(tree, GIT_TREE_PATH_POLICY_CASE_SENSITIVE, NULL, 0);
	/* No invented Unicode case folding: accented names never
	 * collide and submodules are checked like ordinary leaf paths. */
	assert_check(tree, GIT_TREE_PATH_POLICY_WINDOWS, NULL, 0);

	git_tree_free(tree);
}

void test_object_tree_pathcheck__windows_reserved_name_variants(void)
{
	const test_path paths[] = {
		T("COM1"),
		T("aux.txt"),
		T("LpT9.dat"),
		T("prn/file.txt"),
		T("fine10.txt")
	};
	const expected_issue expected[] = {
		P("COM1", GIT_TREE_PATH_ISSUE_RESERVED_NAME),
		P("LpT9.dat", GIT_TREE_PATH_ISSUE_RESERVED_NAME),
		P("aux.txt", GIT_TREE_PATH_ISSUE_RESERVED_NAME),
		P("prn", GIT_TREE_PATH_ISSUE_RESERVED_NAME),
		P("prn/file.txt", GIT_TREE_PATH_ISSUE_RESERVED_NAME)
	};
	git_tree *tree = build_tree(paths, 5);

	assert_check(tree, GIT_TREE_PATH_POLICY_WINDOWS, expected, 5);

	git_tree_free(tree);
}

void test_object_tree_pathcheck__output_sorted_by_git_byte_order(void)
{
	const test_path paths[] = {
		T("z "),
		T("M."),
		T("aux"),
		T("con")
	};
	const expected_issue expected[] = {
		P("M.", GIT_TREE_PATH_ISSUE_TRAILING_DOT),
		P("aux", GIT_TREE_PATH_ISSUE_RESERVED_NAME),
		P("con", GIT_TREE_PATH_ISSUE_RESERVED_NAME),
		P("z ", GIT_TREE_PATH_ISSUE_TRAILING_SPACE)
	};
	git_tree *tree = build_tree(paths, 4);

	/* "M." < "aux" < "con" < "z " by raw byte order. */
	assert_check(tree, GIT_TREE_PATH_POLICY_WINDOWS, expected, 4);

	git_tree_free(tree);
}
