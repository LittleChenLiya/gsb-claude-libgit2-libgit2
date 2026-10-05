#include "clar_libgit2.h"
#include "odb.h"
#include "git2/odb_backend.h"
#include "repository.h"
#include "tree.h"

#define TEST_REPO_PATH "readsession.git"

static git_repository *g_repo;
static git_odb *g_odb;

void test_odb_readsession__initialize(void)
{
	g_repo = cl_git_sandbox_init(TEST_REPO_PATH);
	cl_git_pass(git_repository_odb(&g_odb, g_repo));
}

void test_odb_readsession__cleanup(void)
{
	git_odb_free(g_odb);
	cl_git_sandbox_cleanup();
}

static void write_blob(git_oid *out, const char *data, size_t len)
{
	cl_git_pass(git_odb_write(out, g_odb, data, len, GIT_OBJECT_BLOB));
}

static int count_objects_cb(const git_oid *oid, void *data)
{
	size_t *count = (size_t *)data;
	GIT_UNUSED(oid);
	(*count)++;
	return 0;
}

static size_t count_objects(void)
{
	size_t count = 0;
	cl_git_pass(git_odb_foreach(g_odb, count_objects_cb, &count));
	return count;
}

static void build_tree(git_oid *out, const git_oid *blob1, const git_oid *blob2)
{
	git_treebuilder *builder;

	cl_git_pass(git_treebuilder_new(&builder, g_repo, NULL));
	cl_git_pass(git_treebuilder_insert(NULL, builder, "a.txt", blob1, GIT_FILEMODE_BLOB));
	cl_git_pass(git_treebuilder_insert(NULL, builder, "b.txt", blob2, GIT_FILEMODE_BLOB));
	cl_git_pass(git_treebuilder_write(out, builder));
	git_treebuilder_free(builder);
}

/*
 * A tree points at two small blobs.  The session has a total byte budget
 * only large enough for one of them.  The first blob is delivered; the
 * second is explicitly rejected with an error.  The object store is
 * unchanged by the session.
 */
void test_odb_readsession__budget_rejects_second_blob(void)
{
	git_oid blob1, blob2, tree_id;
	git_odb_read_session *session;
	git_odb_read_session_options opts = GIT_ODB_READ_SESSION_OPTIONS_INIT;
	git_odb_object *obj = NULL;
	size_t before, after;

	write_blob(&blob1, "hello", 5);
	write_blob(&blob2, "world", 5);
	build_tree(&tree_id, &blob1, &blob2);

	/* count objects before the session */
	before = count_objects();

	/* budget for exactly one blob (5 bytes) */
	opts.allowed_types = (1u << GIT_OBJECT_BLOB);
	opts.max_total_size = 5;
	cl_git_pass(git_odb_read_session_new(&session, g_odb, &opts));

	/* the first read is delivered */
	cl_git_pass(git_odb_read_session_read(&obj, session, &blob1));
	cl_assert_equal_sz(git_odb_object_size(obj), 5);
	git_odb_object_free(obj);

	/* the second read exceeds the budget and is rejected */
	cl_git_fail_with(GIT_EINVALID, git_odb_read_session_read(&obj, session, &blob2));
	cl_assert(obj == NULL);

	/* the tree is not a blob, so it is rejected by the type filter */
	cl_git_fail_with(GIT_EINVALID, git_odb_read_session_read(&obj, session, &tree_id));
	cl_assert(obj == NULL);

	/* billing counters reflect only the one delivered object */
	cl_assert_equal_sz(git_odb_read_session_delivered(session), 5);
	cl_assert_equal_sz(git_odb_read_session_object_count(session), 1);

	git_odb_read_session_free(session);

	/* the object store is unchanged */
	after = count_objects();
	cl_assert_equal_sz(before, after);
}

/*
 * The type filter uses the type from the object header, never a guessed
 * type.  A tree is not delivered as a blob even though it is reachable.
 */
void test_odb_readsession__type_filter_uses_header_type(void)
{
	git_oid blob1, blob2, tree_id;
	git_odb_read_session *session;
	git_odb_read_session_options opts = GIT_ODB_READ_SESSION_OPTIONS_INIT;
	git_odb_object *obj = NULL;

	write_blob(&blob1, "one", 3);
	write_blob(&blob2, "two", 3);
	build_tree(&tree_id, &blob1, &blob2);

	/* only blobs are allowed */
	opts.allowed_types = (1u << GIT_OBJECT_BLOB);
	cl_git_pass(git_odb_read_session_new(&session, g_odb, &opts));

	/* a blob is delivered */
	cl_git_pass(git_odb_read_session_read(&obj, session, &blob1));
	cl_assert(git_odb_object_type(obj) == GIT_OBJECT_BLOB);
	git_odb_object_free(obj);

	/* the tree is rejected: its real type is tree, not blob */
	cl_git_fail_with(GIT_EINVALID, git_odb_read_session_read(&obj, session, &tree_id));
	cl_assert(obj == NULL);

	/* an unknown/invalid type is not decoded as an allowed type */
	cl_git_fail_with(GIT_EINVALID, git_odb_read_session_read(&obj, session, &tree_id));

	git_odb_read_session_free(session);
}

/*
 * The per-object byte cap rejects an object larger than the cap, and no
 * bytes are billed for it.
 */
void test_odb_readsession__per_object_cap(void)
{
	git_oid big_id, small_id;
	git_odb_read_session *session;
	git_odb_read_session_options opts = GIT_ODB_READ_SESSION_OPTIONS_INIT;
	git_odb_object *obj = NULL;

	write_blob(&big_id, "this blob is too big", 20);
	write_blob(&small_id, "ok", 2);

	opts.allowed_types = (1u << GIT_OBJECT_BLOB);
	opts.max_object_size = 5;
	cl_git_pass(git_odb_read_session_new(&session, g_odb, &opts));

	/* the small blob is delivered */
	cl_git_pass(git_odb_read_session_read(&obj, session, &small_id));
	cl_assert_equal_sz(git_odb_object_size(obj), 2);
	git_odb_object_free(obj);

	/* the big blob exceeds the per-object cap and is rejected */
	cl_git_fail_with(GIT_EINVALID, git_odb_read_session_read(&obj, session, &big_id));
	cl_assert(obj == NULL);

	/* only the small blob was billed */
	cl_assert_equal_sz(git_odb_read_session_delivered(session), 2);
	cl_assert_equal_sz(git_odb_read_session_object_count(session), 1);

	git_odb_read_session_free(session);
}

/*
 * Duplicate references to the same object: billing rules.
 *
 * A tree references the same blob twice.  Without dedup the blob is
 * delivered (and billed) twice.  With dedup the blob is delivered once
 * (and billed once) but both sources are reported.
 */
typedef struct {
	int delivered;
	int duplicates;
	git_oid last_source;
} traverse_counts;

static int traverse_count_cb(
	git_odb_object *obj,
	const git_oid *source,
	unsigned int flags,
	void *payload)
{
	traverse_counts *counts = (traverse_counts *)payload;

	GIT_UNUSED(obj);

	if (flags & GIT_ODB_READ_SESSION_OBJ_DELIVERED)
		counts->delivered++;
	if (flags & GIT_ODB_READ_SESSION_OBJ_DUPLICATE) {
		counts->duplicates++;
		if (source)
			git_oid_cpy(&counts->last_source, source);
	}
	return 0;
}

void test_odb_readsession__duplicate_reference_billing(void)
{
	git_oid blob_id, tree_id;
	git_odb_read_session *session;
	git_odb_read_session_options opts = GIT_ODB_READ_SESSION_OPTIONS_INIT;
	traverse_counts counts;
	git_treebuilder *builder;

	write_blob(&blob_id, "dup", 3);

	/* build a tree that references the same blob twice under different names */
	cl_git_pass(git_treebuilder_new(&builder, g_repo, NULL));
	cl_git_pass(git_treebuilder_insert(NULL, builder, "first", &blob_id, GIT_FILEMODE_BLOB));
	cl_git_pass(git_treebuilder_insert(NULL, builder, "second", &blob_id, GIT_FILEMODE_BLOB));
	cl_git_pass(git_treebuilder_write(&tree_id, builder));
	git_treebuilder_free(builder);

	/* without dedup: the blob is delivered and billed twice */
	memset(&counts, 0, sizeof(counts));
	opts.traverse = GIT_ODB_TRAVERSE_TREE_BLOBS;
	cl_git_pass(git_odb_read_session_new(&session, g_odb, &opts));
	cl_git_pass(git_odb_read_session_traverse(session, &tree_id, traverse_count_cb, &counts));
	cl_assert_equal_i(counts.delivered, 3);   /* tree + blob + blob */
	cl_assert_equal_i(counts.duplicates, 0);
	/* tree (67 bytes) + blob (3 bytes) delivered twice = 67 + 3 + 3 */
	cl_assert_equal_sz(git_odb_read_session_delivered(session), 67 + 3 + 3);
	cl_assert_equal_sz(git_odb_read_session_object_count(session), 3);
	git_odb_read_session_free(session);

	/* with dedup: the blob is delivered and billed once, both sources reported */
	memset(&counts, 0, sizeof(counts));
	opts.flags = GIT_ODB_READ_SESSION_DEDUP_REFERENCES;
	opts.traverse = GIT_ODB_TRAVERSE_TREE_BLOBS;
	cl_git_pass(git_odb_read_session_new(&session, g_odb, &opts));
	cl_git_pass(git_odb_read_session_traverse(session, &tree_id, traverse_count_cb, &counts));
	cl_assert_equal_i(counts.delivered, 2);   /* tree + blob (once) */
	cl_assert_equal_i(counts.duplicates, 1); /* the second reference */
	/* tree (67 bytes) + blob (3 bytes) once */
	cl_assert_equal_sz(git_odb_read_session_delivered(session), 67 + 3);
	cl_assert_equal_sz(git_odb_read_session_object_count(session), 2);
	git_odb_read_session_free(session);
}

/*
 * Three-object demo: a tree with three blobs, traversed through a session
 * that follows blob edges and has a budget large enough for all three.
 */
void test_odb_readsession__three_object_demo(void)
{
	git_oid blob1, blob2, blob3, tree_id;
	git_odb_read_session *session;
	git_odb_read_session_options opts = GIT_ODB_READ_SESSION_OPTIONS_INIT;
	traverse_counts counts;

	write_blob(&blob1, "one", 3);
	write_blob(&blob2, "two", 3);
	write_blob(&blob3, "three", 5);

	{
		git_treebuilder *builder;
		cl_git_pass(git_treebuilder_new(&builder, g_repo, NULL));
		cl_git_pass(git_treebuilder_insert(NULL, builder, "a", &blob1, GIT_FILEMODE_BLOB));
		cl_git_pass(git_treebuilder_insert(NULL, builder, "b", &blob2, GIT_FILEMODE_BLOB));
		cl_git_pass(git_treebuilder_insert(NULL, builder, "c", &blob3, GIT_FILEMODE_BLOB));
		cl_git_pass(git_treebuilder_write(&tree_id, builder));
		git_treebuilder_free(builder);
	}

	memset(&counts, 0, sizeof(counts));
	opts.allowed_types = (1u << GIT_OBJECT_BLOB) | (1u << GIT_OBJECT_TREE);
	opts.traverse = GIT_ODB_TRAVERSE_TREE_BLOBS;
	opts.max_total_size = 100;
	cl_git_pass(git_odb_read_session_new(&session, g_odb, &opts));
	cl_git_pass(git_odb_read_session_traverse(session, &tree_id, traverse_count_cb, &counts));

	/* the tree and all three blobs are delivered */
	cl_assert_equal_i(counts.delivered, 4);
	cl_assert_equal_i(counts.duplicates, 0);
	cl_assert_equal_sz(git_odb_read_session_object_count(session), 4);
	/* tree (87 bytes) + 3 + 3 + 5 = 98 */
	cl_assert_equal_sz(git_odb_read_session_delivered(session), 87 + 3 + 3 + 5);

	git_odb_read_session_free(session);
}
