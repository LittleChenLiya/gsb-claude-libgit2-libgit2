#include "clar_libgit2.h"
#include "git2/traversal.h"
#include "fs_path.h"
#include "vector.h"
#include "str.h"
#include "posix.h"

static git_repository *repo;

static git_oid tree_id;
static git_oid commit_a; /* root */
static git_oid commit_b; /* child of a */
static git_oid commit_c; /* child of a */
static git_oid commit_m; /* merge of b and c */
static git_oid commit_d; /* child of m */

static void make_commit(git_oid *out, const char *message,
	git_commit **parents, size_t parent_count)
{
	git_signature *sig;
	git_tree *tree;

	cl_git_pass(git_signature_new(&sig, "Traversal", "traversal@example.com",
		1480000000, 0));
	cl_git_pass(git_tree_lookup(&tree, repo, &tree_id));
	cl_git_pass(git_commit_create(out, repo, NULL, sig, sig, NULL,
		message, tree, parent_count, (const git_commit **)parents));

	git_tree_free(tree);
	git_signature_free(sig);
}

void test_traversal_report__initialize(void)
{
	git_treebuilder *builder;
	git_commit *a, *b, *c, *m;
	git_commit *merge_parents[2];

	repo = cl_git_sandbox_init_new("traversal");

	cl_git_pass(git_treebuilder_new(&builder, repo, NULL));
	cl_git_pass(git_treebuilder_write(&tree_id, builder));
	git_treebuilder_free(builder);

	make_commit(&commit_a, "root", NULL, 0);

	cl_git_pass(git_commit_lookup(&a, repo, &commit_a));
	make_commit(&commit_b, "second", &a, 1);
	make_commit(&commit_c, "third", &a, 1);

	cl_git_pass(git_commit_lookup(&b, repo, &commit_b));
	cl_git_pass(git_commit_lookup(&c, repo, &commit_c));
	merge_parents[0] = b;
	merge_parents[1] = c;
	make_commit(&commit_m, "merge", merge_parents, 2);

	cl_git_pass(git_commit_lookup(&m, repo, &commit_m));
	make_commit(&commit_d, "tip", &m, 1);

	git_commit_free(a);
	git_commit_free(b);
	git_commit_free(c);
	git_commit_free(m);
}

void test_traversal_report__cleanup(void)
{
	cl_git_sandbox_cleanup();
}

typedef struct {
	git_traversal_event_t event;
	git_traversal_boundary_reason_t reason;
	git_oid commit;
	git_oid parent;
	bool has_parent;
} recorded_event;

static recorded_event events[128];
static size_t event_count;

static int record_event(
	git_traversal_event_t event,
	const git_oid *commit,
	const git_oid *parent,
	git_traversal_boundary_reason_t reason,
	void *payload)
{
	recorded_event *recorded;

	GIT_UNUSED(payload);

	cl_assert(event_count < sizeof(events) / sizeof(events[0]));

	recorded = &events[event_count++];
	recorded->event = event;
	recorded->reason = reason;
	git_oid_cpy(&recorded->commit, commit);
	recorded->has_parent = (parent != NULL);
	if (parent != NULL)
		git_oid_cpy(&recorded->parent, parent);

	return 0;
}

static void run_report(
	const git_oid *starts, size_t starts_len,
	const git_oid *stops, size_t stops_len)
{
	event_count = 0;
	cl_git_pass(git_traversal_report(repo, starts, starts_len,
		stops, stops_len, record_event, NULL));
}

static size_t count_events(
	git_traversal_event_t event,
	git_traversal_boundary_reason_t reason)
{
	size_t i, count = 0;

	for (i = 0; i < event_count; i++) {
		if (events[i].event == event && events[i].reason == reason)
			count++;
	}

	return count;
}

static size_t count_commit_events(const git_oid *oid)
{
	size_t i, count = 0;

	for (i = 0; i < event_count; i++) {
		if (events[i].event == GIT_TRAVERSAL_EVENT_COMMIT &&
		    git_oid_equal(&events[i].commit, oid))
			count++;
	}

	return count;
}

static bool has_boundary(
	const git_oid *commit,
	const git_oid *parent,
	git_traversal_boundary_reason_t reason)
{
	size_t i;

	for (i = 0; i < event_count; i++) {
		if (events[i].event != GIT_TRAVERSAL_EVENT_BOUNDARY ||
		    events[i].reason != reason ||
		    !git_oid_equal(&events[i].commit, commit))
			continue;

		if (parent == NULL && !events[i].has_parent)
			return true;

		if (parent != NULL && events[i].has_parent &&
		    git_oid_equal(&events[i].parent, parent))
			return true;
	}

	return false;
}

static void assert_commits_visited_once(const git_oid *expected, size_t expected_len)
{
	size_t i;

	cl_assert_equal_i(expected_len,
		count_events(GIT_TRAVERSAL_EVENT_COMMIT, GIT_TRAVERSAL_BOUNDARY_NONE));

	for (i = 0; i < expected_len; i++)
		cl_assert_equal_i(1, count_commit_events(&expected[i]));
}

void test_traversal_report__full_traversal_visits_merge_and_shared_ancestor_once(void)
{
	git_oid starts[] = { commit_d };
	git_oid expected[] = { commit_a, commit_b, commit_c, commit_m, commit_d };

	run_report(starts, 1, NULL, 0);

	assert_commits_visited_once(expected, 5);
	cl_assert_equal_i(0, count_events(GIT_TRAVERSAL_EVENT_BOUNDARY,
		GIT_TRAVERSAL_BOUNDARY_STOP));
	cl_assert_equal_i(0, count_events(GIT_TRAVERSAL_EVENT_BOUNDARY,
		GIT_TRAVERSAL_BOUNDARY_MISSING));
	cl_assert_equal_i(0, count_events(GIT_TRAVERSAL_EVENT_BOUNDARY,
		GIT_TRAVERSAL_BOUNDARY_ERROR));
}

void test_traversal_report__stop_commit_is_reported_as_boundary(void)
{
	git_oid starts[] = { commit_d };
	git_oid stops[] = { commit_b };
	git_oid expected[] = { commit_a, commit_c, commit_d, commit_m };

	run_report(starts, 1, stops, 1);

	assert_commits_visited_once(expected, 4);
	cl_assert(has_boundary(&commit_m, &commit_b, GIT_TRAVERSAL_BOUNDARY_STOP));
	cl_assert_equal_i(1, count_events(GIT_TRAVERSAL_EVENT_BOUNDARY,
		GIT_TRAVERSAL_BOUNDARY_STOP));
}

void test_traversal_report__stop_at_shared_ancestor_reports_each_edge(void)
{
	git_oid starts[] = { commit_d };
	git_oid stops[] = { commit_a };
	git_oid expected[] = { commit_b, commit_c, commit_d, commit_m };

	run_report(starts, 1, stops, 1);

	assert_commits_visited_once(expected, 4);
	cl_assert(has_boundary(&commit_b, &commit_a, GIT_TRAVERSAL_BOUNDARY_STOP));
	cl_assert(has_boundary(&commit_c, &commit_a, GIT_TRAVERSAL_BOUNDARY_STOP));
	cl_assert_equal_i(2, count_events(GIT_TRAVERSAL_EVENT_BOUNDARY,
		GIT_TRAVERSAL_BOUNDARY_STOP));
}

void test_traversal_report__start_that_is_a_stop_is_a_boundary(void)
{
	git_oid starts[] = { commit_d };
	git_oid stops[] = { commit_d };

	run_report(starts, 1, stops, 1);

	cl_assert_equal_i(0, count_events(GIT_TRAVERSAL_EVENT_COMMIT,
		GIT_TRAVERSAL_BOUNDARY_NONE));
	cl_assert(has_boundary(&commit_d, NULL, GIT_TRAVERSAL_BOUNDARY_STOP));
	cl_assert_equal_i(1, count_events(GIT_TRAVERSAL_EVENT_BOUNDARY,
		GIT_TRAVERSAL_BOUNDARY_STOP));
}

void test_traversal_report__true_root_has_no_boundary(void)
{
	git_oid starts[] = { commit_a };
	git_oid expected[] = { commit_a };

	run_report(starts, 1, NULL, 0);

	assert_commits_visited_once(expected, 1);
	cl_assert_equal_i(0, event_count - 1);
}

void test_traversal_report__multiple_starts_share_history(void)
{
	git_oid starts[] = { commit_d, commit_c };
	git_oid expected[] = { commit_a, commit_b, commit_c, commit_d, commit_m };

	run_report(starts, 2, NULL, 0);

	assert_commits_visited_once(expected, 5);
	cl_assert_equal_i(5, event_count);
}

static void loose_object_path(git_str *out, const git_oid *oid)
{
	char hex[GIT_OID_SHA1_HEXSIZE + 1];

	git_oid_fmt(hex, oid);
	hex[GIT_OID_SHA1_HEXSIZE] = '\0';

	git_str_clear(out);
	git_str_printf(out, "traversal/.git/objects/%.2s/%s", hex, hex + 2);
}

static void snapshot_walk(git_vector *entries, const char *dir)
{
	git_fs_path_diriter iter = GIT_FS_PATH_DIRITER_INIT;
	const char *fullpath;
	size_t fullpath_len;
	struct stat st;

	cl_git_pass(git_fs_path_diriter_init(&iter, dir, 0));

	while (git_fs_path_diriter_next(&iter) == 0) {
		cl_git_pass(git_fs_path_diriter_fullpath(&fullpath, &fullpath_len, &iter));
		cl_git_pass(git_fs_path_diriter_stat(&st, &iter));

		if (S_ISDIR(st.st_mode)) {
			snapshot_walk(entries, fullpath);
		} else {
			git_str entry = GIT_STR_INIT;
			git_str_printf(&entry, "%s %ld %ld",
				fullpath, (long)st.st_mtime, (long)st.st_size);
			cl_git_pass(git_vector_insert(entries,
				git__strdup(git_str_cstr(&entry))));
			git_str_dispose(&entry);
		}
	}

	git_fs_path_diriter_free(&iter);
}

static void snapshot_repo(git_str *out)
{
	git_vector entries = GIT_VECTOR_INIT;
	size_t i;
	char *entry;

	git_str_clear(out);
	snapshot_walk(&entries, "traversal");
	git_vector_sort(&entries);

	git_vector_foreach(&entries, i, entry) {
		git_str_puts(out, entry);
		git_str_putc(out, '\n');
		git__free(entry);
	}

	git_vector_dispose(&entries);
}

static void assert_repo_unmodified(const git_str *before)
{
	git_str after = GIT_STR_INIT;

	snapshot_repo(&after);
	cl_assert_equal_s(git_str_cstr(before), git_str_cstr(&after));

	git_str_dispose(&after);
}

void test_traversal_report__missing_parent_object_is_reported(void)
{
	git_oid starts[] = { commit_d };
	git_oid expected[] = { commit_a, commit_c, commit_d, commit_m };
	git_str object_path = GIT_STR_INIT, before = GIT_STR_INIT;

	loose_object_path(&object_path, &commit_b);
	cl_assert(git_fs_path_exists(git_str_cstr(&object_path)));
	cl_must_pass(p_chmod(git_str_cstr(&object_path), 0666));
	cl_must_pass(p_unlink(git_str_cstr(&object_path)));

	/* drop any cached objects so the deletion is observed */
	repo = cl_git_sandbox_reopen();

	snapshot_repo(&before);
	run_report(starts, 1, NULL, 0);

	assert_commits_visited_once(expected, 4);
	cl_assert(has_boundary(&commit_m, &commit_b,
		GIT_TRAVERSAL_BOUNDARY_MISSING));
	cl_assert_equal_i(1, count_events(GIT_TRAVERSAL_EVENT_BOUNDARY,
		GIT_TRAVERSAL_BOUNDARY_MISSING));

	assert_repo_unmodified(&before);

	git_str_dispose(&object_path);
	git_str_dispose(&before);
}

void test_traversal_report__corrupt_parent_object_is_reported(void)
{
	git_oid starts[] = { commit_d };
	git_oid expected[] = { commit_a, commit_c, commit_d, commit_m };
	git_str object_path = GIT_STR_INIT, before = GIT_STR_INIT;

	loose_object_path(&object_path, &commit_b);
	cl_assert(git_fs_path_exists(git_str_cstr(&object_path)));
	cl_must_pass(p_chmod(git_str_cstr(&object_path), 0666));
	cl_git_rewritefile(git_str_cstr(&object_path),
		"this is not a valid git object");

	/* drop any cached objects so the corruption is observed */
	repo = cl_git_sandbox_reopen();

	snapshot_repo(&before);
	run_report(starts, 1, NULL, 0);

	assert_commits_visited_once(expected, 4);
	cl_assert(has_boundary(&commit_m, &commit_b,
		GIT_TRAVERSAL_BOUNDARY_ERROR));
	cl_assert_equal_i(1, count_events(GIT_TRAVERSAL_EVENT_BOUNDARY,
		GIT_TRAVERSAL_BOUNDARY_ERROR));

	assert_repo_unmodified(&before);

	git_str_dispose(&object_path);
	git_str_dispose(&before);
}

void test_traversal_report__unknown_start_oid_is_missing(void)
{
	git_oid starts[1];

	cl_git_pass(git_oid_from_string(&starts[0],
		"1111111111111111111111111111111111111111", GIT_OID_SHA1));

	run_report(starts, 1, NULL, 0);

	cl_assert_equal_i(0, count_events(GIT_TRAVERSAL_EVENT_COMMIT,
		GIT_TRAVERSAL_BOUNDARY_NONE));
	cl_assert(has_boundary(&starts[0], NULL, GIT_TRAVERSAL_BOUNDARY_MISSING));
	cl_assert_equal_i(1, event_count);
}

void test_traversal_report__non_commit_start_oid_is_an_error(void)
{
	git_oid starts[1];

	cl_git_pass(git_blob_create_from_buffer(&starts[0], repo, "blob", 4));

	run_report(starts, 1, NULL, 0);

	cl_assert_equal_i(0, count_events(GIT_TRAVERSAL_EVENT_COMMIT,
		GIT_TRAVERSAL_BOUNDARY_NONE));
	cl_assert(has_boundary(&starts[0], NULL, GIT_TRAVERSAL_BOUNDARY_ERROR));
	cl_assert_equal_i(1, event_count);
}

static int count_ref(const char *name, void *payload)
{
	size_t *count = payload;

	GIT_UNUSED(name);
	(*count)++;
	return 0;
}

void test_traversal_report__does_not_modify_refs_index_or_workdir(void)
{
	git_oid starts[] = { commit_d, commit_c };
	git_oid stops[] = { commit_b };
	git_str before = GIT_STR_INIT;
	size_t ref_count = 0;

	snapshot_repo(&before);

	run_report(starts, 2, stops, 1);

	assert_repo_unmodified(&before);

	/* the traversal must not create references or an index */
	cl_git_pass(git_reference_foreach_name(repo, count_ref, &ref_count));
	cl_assert_equal_i(0, ref_count);
	cl_assert(!git_fs_path_exists("traversal/.git/index"));

	git_str_dispose(&before);
}
