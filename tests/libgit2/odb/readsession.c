#include "clar_libgit2.h"
#include "git2/sys/mempack.h"

static git_odb *_odb;
static git_odb_backend *_mempack;
static git_oid _id_commit, _id_tree, _id_blob1, _id_blob2;
static size_t _commit_len, _tree_len;

#define BLOB1_DATA "hello"
#define BLOB2_DATA "world!"
#define BLOB1_LEN 5
#define BLOB2_LEN 6

void test_odb_readsession__initialize(void)
{
	git_str tree = GIT_STR_INIT, commit = GIT_STR_INIT;
	char tree_hex[GIT_OID_SHA1_HEXSIZE + 1];

	cl_git_pass(git_mempack_new(&_mempack));
	cl_git_pass(git_odb_new(&_odb));
	cl_git_pass(git_odb_add_backend(_odb, _mempack, 10));

	cl_git_pass(git_odb_write(&_id_blob1, _odb, BLOB1_DATA, BLOB1_LEN, GIT_OBJECT_BLOB));
	cl_git_pass(git_odb_write(&_id_blob2, _odb, BLOB2_DATA, BLOB2_LEN, GIT_OBJECT_BLOB));

	git_str_puts(&tree, "100644 one");
	git_str_putc(&tree, '\0');
	git_str_put(&tree, (const char *)_id_blob1.id, GIT_OID_SHA1_SIZE);
	git_str_puts(&tree, "100644 two");
	git_str_putc(&tree, '\0');
	git_str_put(&tree, (const char *)_id_blob2.id, GIT_OID_SHA1_SIZE);
	_tree_len = git_str_len(&tree);
	cl_git_pass(git_odb_write(&_id_tree, _odb, tree.ptr, _tree_len, GIT_OBJECT_TREE));

	cl_git_pass(git_oid_fmt(tree_hex, &_id_tree));
	tree_hex[GIT_OID_SHA1_HEXSIZE] = '\0';
	git_str_printf(&commit,
	    "tree %s\n"
	    "author T <t@example.com> 1234567890 +0000\n"
	    "committer T <t@example.com> 1234567890 +0000\n"
	    "\n"
	    "three-object demo\n",
	    tree_hex);
	_commit_len = git_str_len(&commit);
	cl_git_pass(git_odb_write(&_id_commit, _odb, commit.ptr, _commit_len, GIT_OBJECT_COMMIT));

	git_str_dispose(&tree);
	git_str_dispose(&commit);
}

void test_odb_readsession__cleanup(void)
{
	git_odb_free(_odb); /* also frees the mempack backend */
	_odb = NULL;
	_mempack = NULL;
}

static git_odb_read_session *make_session(
	uint32_t types, uint64_t max_obj, uint64_t max_total)
{
	git_odb_read_session_options opts = GIT_ODB_READ_SESSION_OPTIONS_INIT;
	git_odb_read_session *session = NULL;

	opts.allowed_types = types;
	opts.max_object_bytes = max_obj;
	opts.max_total_bytes = max_total;

	cl_git_pass(git_odb_read_session_open(&session, _odb, &opts));
	return session;
}

/* the default ODB read path and the object store itself are unchanged */
static void assert_odb_intact(void)
{
	git_odb_object *obj;

	cl_assert(git_odb_exists(_odb, &_id_blob1));
	cl_assert(git_odb_exists(_odb, &_id_blob2));
	cl_assert(git_odb_exists(_odb, &_id_tree));
	cl_assert(git_odb_exists(_odb, &_id_commit));

	cl_git_pass(git_odb_read(&obj, _odb, &_id_blob1));
	cl_assert_equal_i(GIT_OBJECT_BLOB, git_odb_object_type(obj));
	cl_assert_equal_sz(BLOB1_LEN, git_odb_object_size(obj));
	git_odb_object_free(obj);

	cl_git_pass(git_odb_read(&obj, _odb, &_id_blob2));
	cl_assert_equal_sz(BLOB2_LEN, git_odb_object_size(obj));
	git_odb_object_free(obj);

	cl_git_pass(git_odb_read(&obj, _odb, &_id_tree));
	cl_assert_equal_i(GIT_OBJECT_TREE, git_odb_object_type(obj));
	git_odb_object_free(obj);

	cl_git_pass(git_odb_read(&obj, _odb, &_id_commit));
	cl_assert_equal_i(GIT_OBJECT_COMMIT, git_odb_object_type(obj));
	git_odb_object_free(obj);
}

void test_odb_readsession__total_budget_refuses_second_blob(void)
{
	git_odb_read_session *session;
	git_odb_object *obj = NULL;

	/* the tree points at two small blobs; the total budget only
	 * covers one of them */
	session = make_session(GIT_ODB_READ_SESSION_TYPE_BLOB, 0, BLOB1_LEN);

	cl_git_pass(git_odb_read_session_read(&obj, session, &_id_blob1));
	cl_assert_equal_i(GIT_OBJECT_BLOB, git_odb_object_type(obj));
	cl_assert_equal_sz(BLOB1_LEN, git_odb_object_size(obj));
	git_odb_object_free(obj);
	obj = NULL;
	cl_assert_equal_i(BLOB1_LEN, git_odb_read_session_spent(session));

	/* the second read is explicitly refused and delivers nothing */
	cl_git_fail_with(GIT_EBUDGET,
		git_odb_read_session_read(&obj, session, &_id_blob2));
	cl_assert(obj == NULL);
	cl_assert_equal_i(BLOB1_LEN, git_odb_read_session_spent(session));

	git_odb_read_session_free(session);

	/* the original object database is unchanged */
	assert_odb_intact();
}

void test_odb_readsession__wrong_type_is_rejected_not_decoded(void)
{
	git_odb_read_session *session;
	git_odb_object *obj = NULL;

	session = make_session(GIT_ODB_READ_SESSION_TYPE_BLOB, 0, 0);

	/* a tree or commit must not be decoded as an allowed blob */
	cl_git_fail_with(GIT_EINVALID,
		git_odb_read_session_read(&obj, session, &_id_tree));
	cl_assert(obj == NULL);
	cl_git_fail_with(GIT_EINVALID,
		git_odb_read_session_read(&obj, session, &_id_commit));
	cl_assert(obj == NULL);

	/* rejections deliver nothing and bill nothing */
	cl_assert_equal_i(0, git_odb_read_session_spent(session));

	/* the allowed type still reads fine */
	cl_git_pass(git_odb_read_session_read(&obj, session, &_id_blob1));
	cl_assert_equal_i(GIT_OBJECT_BLOB, git_odb_object_type(obj));
	git_odb_object_free(obj);

	git_odb_read_session_free(session);
}

void test_odb_readsession__per_object_budget(void)
{
	git_odb_read_session *session;
	git_odb_object *obj = NULL;

	session = make_session(0, BLOB1_LEN, 0);

	/* exactly at the budget is fine */
	cl_git_pass(git_odb_read_session_read(&obj, session, &_id_blob1));
	git_odb_object_free(obj);
	obj = NULL;

	/* one byte over the per-object budget is refused */
	cl_git_fail_with(GIT_EBUDGET,
		git_odb_read_session_read(&obj, session, &_id_blob2));
	cl_assert(obj == NULL);
	cl_assert_equal_i(BLOB1_LEN, git_odb_read_session_spent(session));

	git_odb_read_session_free(session);
}

void test_odb_readsession__duplicate_reference_billing(void)
{
	git_odb_read_session *session;
	git_odb_object *obj = NULL;
	git_oid first_source;

	/* direct reads bill every delivery, including cache hits */
	session = make_session(0, 0, 0);
	cl_git_pass(git_odb_read_session_read(&obj, session, &_id_blob1));
	git_odb_object_free(obj);
	obj = NULL;
	cl_git_pass(git_odb_read_session_read(&obj, session, &_id_blob1));
	git_odb_object_free(obj);
	obj = NULL;
	cl_assert_equal_i(2 * BLOB1_LEN, git_odb_read_session_spent(session));
	git_odb_read_session_free(session);

	/* within a traversal, repeated references to the same object are
	 * deduplicated: not billed again, but the source is reported */
	session = make_session(0, 0, 0);
	cl_git_pass(git_odb_read_session_follow(&obj, NULL, session, &_id_tree, &_id_blob1));
	git_odb_object_free(obj);
	obj = NULL;
	cl_assert_equal_i(BLOB1_LEN, git_odb_read_session_spent(session));

	cl_git_fail_with(GIT_ITEROVER,
		git_odb_read_session_follow(&obj, &first_source, session, &_id_tree, &_id_blob1));
	cl_assert(obj == NULL);
	cl_assert(git_oid_equal(&first_source, &_id_tree));
	cl_assert_equal_i(BLOB1_LEN, git_odb_read_session_spent(session));

	/* a repeated reference through a different edge still reports
	 * the original source */
	cl_git_fail_with(GIT_ITEROVER,
		git_odb_read_session_follow(NULL, &first_source, session, &_id_commit, &_id_blob1));
	cl_assert(git_oid_equal(&first_source, &_id_tree));
	cl_assert_equal_i(BLOB1_LEN, git_odb_read_session_spent(session));

	git_odb_read_session_free(session);
}

void test_odb_readsession__three_object_walk(void)
{
	uint64_t budget = (uint64_t)_commit_len + _tree_len + BLOB1_LEN;
	git_odb_read_session *session;
	git_odb_object *obj = NULL;

	session = make_session(
		GIT_ODB_READ_SESSION_TYPE_COMMIT |
		GIT_ODB_READ_SESSION_TYPE_TREE |
		GIT_ODB_READ_SESSION_TYPE_BLOB,
		0, budget);

	/* the traversal only follows caller-selected edges:
	 * commit -> tree -> first blob */
	cl_git_pass(git_odb_read_session_follow(&obj, NULL, session, NULL, &_id_commit));
	cl_assert_equal_i(GIT_OBJECT_COMMIT, git_odb_object_type(obj));
	git_odb_object_free(obj);
	obj = NULL;

	cl_git_pass(git_odb_read_session_follow(&obj, NULL, session, &_id_commit, &_id_tree));
	cl_assert_equal_i(GIT_OBJECT_TREE, git_odb_object_type(obj));
	git_odb_object_free(obj);
	obj = NULL;

	cl_git_pass(git_odb_read_session_follow(&obj, NULL, session, &_id_tree, &_id_blob1));
	cl_assert_equal_i(GIT_OBJECT_BLOB, git_odb_object_type(obj));
	git_odb_object_free(obj);
	obj = NULL;

	cl_assert_equal_i(budget, git_odb_read_session_spent(session));

	/* the budget is now exactly exhausted; following the edge to the
	 * second blob is refused and delivers nothing */
	cl_git_fail_with(GIT_EBUDGET,
		git_odb_read_session_follow(&obj, NULL, session, &_id_tree, &_id_blob2));
	cl_assert(obj == NULL);
	cl_assert_equal_i(budget, git_odb_read_session_spent(session));

	git_odb_read_session_free(session);

	assert_odb_intact();
}
