/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "git2/traversal.h"

#include "common.h"
#include "commit.h"
#include "errors.h"
#include "hashmap_oid.h"
#include "odb.h"
#include "pool.h"
#include "repository.h"
#include "array.h"

GIT_HASHMAP_OID_SETUP(git_traversal__oidset, char);

typedef struct {
	git_repository *repo;
	git_odb *odb;
	const git_oid *stops;
	size_t stops_len;
	git_traversal_cb callback;
	void *payload;

	git_traversal__oidset visited;
	git_pool oid_pool;
	git_array_t(git_commit *) pending;
} traversal_state;

static bool oid_in_set(const git_oid *set, size_t set_len, const git_oid *oid)
{
	size_t i;

	for (i = 0; i < set_len; i++) {
		if (git_oid_equal(&set[i], oid))
			return true;
	}

	return false;
}

static int emit_event(
	traversal_state *state,
	git_traversal_event_t event,
	const git_oid *commit,
	const git_oid *parent,
	git_traversal_boundary_reason_t reason)
{
	int error = state->callback(event, commit, parent, reason, state->payload);

	if (error != 0)
		git_error_set_after_callback(error);

	return error;
}

static int emit_boundary(
	traversal_state *state,
	const git_oid *from,
	const git_oid *to,
	git_traversal_boundary_reason_t reason)
{
	/* clear any error left behind by a failed object lookup */
	git_error_clear();

	if (from != NULL)
		return emit_event(state, GIT_TRAVERSAL_EVENT_BOUNDARY,
			from, to, reason);
	else
		return emit_event(state, GIT_TRAVERSAL_EVENT_BOUNDARY,
			to, NULL, reason);
}

static int mark_visited(traversal_state *state, const git_oid *oid)
{
	git_oid *cached = git_pool_malloc(&state->oid_pool, 1);

	GIT_ERROR_CHECK_ALLOC(cached);

	git_oid_cpy(cached, oid);

	return git_traversal__oidset_put(&state->visited, cached, 1);
}

static int push_commit(traversal_state *state, git_commit *commit)
{
	git_commit **slot = git_array_alloc(state->pending);

	GIT_ERROR_CHECK_ALLOC(slot);

	*slot = commit;
	return 0;
}

/*
 * Consider a commit reachable from `from` (or a starting point when
 * `from` is NULL): report it as a boundary if traversal cannot or
 * should not proceed past it, otherwise report and queue it.
 */
static int consider_commit(
	traversal_state *state,
	const git_oid *from,
	const git_oid *oid)
{
	git_commit *commit;
	int error;

	if (oid_in_set(state->stops, state->stops_len, oid))
		return emit_boundary(state, from, oid,
			GIT_TRAVERSAL_BOUNDARY_STOP);

	if (git_traversal__oidset_contains(&state->visited, oid))
		return 0;

	error = git_commit_lookup(&commit, state->repo, oid);

	if (error == GIT_ENOTFOUND) {
		/*
		 * A lookup may fail with GIT_ENOTFOUND either because the
		 * object is absent from the object database or because it
		 * is not a commit; disambiguate the two.
		 */
		if (git_odb_exists(state->odb, oid))
			return emit_boundary(state, from, oid,
				GIT_TRAVERSAL_BOUNDARY_ERROR);

		return emit_boundary(state, from, oid,
			GIT_TRAVERSAL_BOUNDARY_MISSING);
	} else if (error < 0) {
		return emit_boundary(state, from, oid,
			GIT_TRAVERSAL_BOUNDARY_ERROR);
	}

	if ((error = mark_visited(state, oid)) < 0 ||
	    (error = emit_event(state, GIT_TRAVERSAL_EVENT_COMMIT,
		oid, NULL, GIT_TRAVERSAL_BOUNDARY_NONE)) != 0 ||
	    (error = push_commit(state, commit)) < 0) {
		git_commit_free(commit);
		return error;
	}

	return 0;
}

int git_traversal_report(
	git_repository *repo,
	const git_oid *starts,
	size_t starts_len,
	const git_oid *stops,
	size_t stops_len,
	git_traversal_cb callback,
	void *payload)
{
	traversal_state state;
	git_commit *commit;
	size_t i;
	int error = 0;

	GIT_ASSERT_ARG(repo);
	GIT_ASSERT_ARG(starts || starts_len == 0);
	GIT_ASSERT_ARG(stops || stops_len == 0);
	GIT_ASSERT_ARG(callback);

	memset(&state, 0, sizeof(state));
	state.repo = repo;
	state.stops = stops;
	state.stops_len = stops_len;
	state.callback = callback;
	state.payload = payload;

	if (git_pool_init(&state.oid_pool, sizeof(git_oid)) < 0)
		return -1;

	if ((error = git_repository_odb(&state.odb, repo)) < 0) {
		git_pool_clear(&state.oid_pool);
		return error;
	}

	for (i = 0; i < starts_len; i++) {
		if ((error = consider_commit(&state, NULL, &starts[i])) != 0)
			goto done;
	}

	while (git_array_size(state.pending) > 0) {
		unsigned int parent_idx, parent_count;
		const git_oid *commit_id;
		git_commit **top = git_array_pop(state.pending);

		GIT_ASSERT(top != NULL);
		commit = *top;
		commit_id = git_commit_id(commit);
		parent_count = git_commit_parentcount(commit);

		for (parent_idx = 0; parent_idx < parent_count; parent_idx++) {
			const git_oid *parent_id =
				git_commit_parent_id(commit, parent_idx);

			if ((error = consider_commit(&state, commit_id, parent_id)) != 0)
				break;
		}

		git_commit_free(commit);

		if (error != 0)
			goto done;
	}

	done:
	while (git_array_size(state.pending) > 0) {
		git_commit **top = git_array_pop(state.pending);

		git_commit_free(*top);
	}

	git_array_dispose(state.pending);
	git_traversal__oidset_dispose(&state.visited);
	git_pool_clear(&state.oid_pool);
	git_odb_free(state.odb);

	return error;
}
