/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "common.h"

#include "git2/pathhistory.h"
#include "git2/commit.h"
#include "git2/tree.h"
#include "git2/diff.h"
#include "git2/repository.h"

#include "alloc.h"
#include "hashmap_str.h"
#include "pool.h"
#include "str.h"
#include "vector.h"

#define DEFAULT_FIND_FLAGS \
	(GIT_DIFF_FIND_RENAMES | GIT_DIFF_FIND_COPIES | \
	 GIT_DIFF_FIND_COPIES_FROM_UNMODIFIED)

/* a (commit, path) position that still has to be examined */
typedef struct {
	git_oid commit_id;
	char *path;
	unsigned int parent_index;
} path_history_state;

typedef struct {
	git_repository *repo;
	git_vector stack;          /* of path_history_state *, LIFO */
	git_hashset_str visited;   /* oid/path keys, owned by pool */
	git_pool pool;
	git_path_history_options opts;
	size_t emitted;
	git_path_history_cb callback;
	void *payload;
} path_history_walk;

static void state_free(path_history_state *state)
{
	if (!state)
		return;
	git__free(state->path);
	git__free(state);
}

static void walk_dispose(path_history_walk *walk)
{
	path_history_state *state;
	size_t i;

	git_vector_foreach(&walk->stack, i, state)
		state_free(state);
	git_vector_dispose(&walk->stack);
	git_hashset_str_dispose(&walk->visited);
	git_pool_clear(&walk->pool);
}

/*
 * Mark a (commit, path) pair as visited; returns 1 if it had already
 * been visited, 0 if it is new, < 0 on error.
 */
static int walk_mark_visited(
	path_history_walk *walk,
	const git_oid *commit_id,
	const char *path)
{
	char hex[GIT_OID_MAX_HEXSIZE + 1];
	git_str key = GIT_STR_INIT;
	const char *pooled;
	int error = 0;

	git_oid_tostr(hex, sizeof(hex), commit_id);

	if (git_str_join(&key, '/', hex, path) < 0) {
		error = -1;
		goto done;
	}

	if (git_hashset_str_contains(&walk->visited, key.ptr)) {
		error = 1;
		goto done;
	}

	if ((pooled = git_pool_strdup_safe(&walk->pool, key.ptr)) == NULL) {
		error = -1;
		goto done;
	}

	if (git_hashset_str_add(&walk->visited, pooled) < 0) {
		error = -1;
		goto done;
	}

done:
	git_str_dispose(&key);
	return error;
}

/* enqueue a new position unless it has been visited before */
static int walk_push(
	path_history_walk *walk,
	const git_oid *commit_id,
	const char *path,
	unsigned int parent_index)
{
	path_history_state *state;
	int error;

	if ((error = walk_mark_visited(walk, commit_id, path)) != 0)
		return error < 0 ? error : 0;

	state = git__malloc(sizeof(path_history_state));
	GIT_ERROR_CHECK_ALLOC(state);

	git_oid_cpy(&state->commit_id, commit_id);
	state->path = git__strdup(path);
	state->parent_index = parent_index;
	if (!state->path) {
		state_free(state);
		git_error_set_oom();
		return -1;
	}

	if ((error = git_vector_insert(&walk->stack, state)) < 0) {
		state_free(state);
		return error;
	}

	return 0;
}

/*
 * Deliver an entry to the user callback.
 *
 * Returns 1 if the entry limit has been reached and the walk must
 * stop (flagging a truncated result), GIT_EUSER if the callback
 * cancelled the walk, or 0 on success.
 */
static int walk_emit(path_history_walk *walk, git_path_history_entry *entry)
{
	if (walk->opts.max_entries && walk->emitted >= walk->opts.max_entries)
		return 1;

	walk->emitted++;

	if (walk->callback(entry, walk->payload) != 0) {
		git_error_set(GIT_ERROR_CALLBACK,
			"path history walk cancelled by user callback");
		return GIT_EUSER;
	}

	return 0;
}

/* allocate a follow-up position and append it to the new_states vector */
static int collect_state(
	git_vector *new_states,
	const git_oid *commit_id,
	const char *path,
	unsigned int parent_index)
{
	path_history_state *next;
	int error;

	next = git__malloc(sizeof(path_history_state));
	GIT_ERROR_CHECK_ALLOC(next);

	git_oid_cpy(&next->commit_id, commit_id);
	next->path = git__strdup(path);
	next->parent_index = parent_index;
	if (!next->path) {
		state_free(next);
		git_error_set_oom();
		return -1;
	}

	if ((error = git_vector_insert(new_states, next)) < 0) {
		state_free(next);
		return error;
	}

	return 0;
}

/*
 * The path does not exist in the parent tree; diff the two trees and
 * use rename/copy detection to figure out where it came from.
 */
static int process_missing_in_parent(
	path_history_walk *walk,
	git_tree *tree,
	git_tree *parent_tree,
	const git_oid *parent_id,
	path_history_state *state,
	const git_oid *blob_id,
	unsigned int parent_index,
	git_vector *new_states)
{
	git_diff_options diffopts = GIT_DIFF_OPTIONS_INIT;
	git_diff *diff = NULL;
	const git_diff_delta *match = NULL;
	git_path_history_entry entry;
	size_t i, ndeltas;
	int error = 0;

	/*
	 * include unmodified entries so that they can serve as copy
	 * sources for GIT_DIFF_FIND_COPIES_FROM_UNMODIFIED
	 */
	diffopts.flags = GIT_DIFF_INCLUDE_UNMODIFIED | GIT_DIFF_SKIP_BINARY_CHECK;

	if ((error = git_diff_tree_to_tree(&diff, walk->repo, parent_tree, tree, &diffopts)) < 0)
		goto done;

	if ((error = git_diff_find_similar(diff, &walk->opts.find_opts)) < 0)
		goto done;

	ndeltas = git_diff_num_deltas(diff);
	for (i = 0; i < ndeltas; i++) {
		const git_diff_delta *delta = git_diff_get_delta(diff, i);

		if (delta->new_file.path &&
		    strcmp(delta->new_file.path, state->path) == 0) {
			match = delta;
			break;
		}
	}

	memset(&entry, 0, sizeof(entry));
	git_oid_cpy(&entry.commit_id, &state->commit_id);
	entry.path = state->path;
	git_oid_cpy(&entry.blob_id, blob_id);
	entry.parent_index = parent_index;

	if (match && match->status == GIT_DELTA_RENAMED) {
		entry.type = GIT_PATH_HISTORY_ENTRY_RENAMED;
		entry.old_path = match->old_file.path;
		git_oid_cpy(&entry.old_blob_id, &match->old_file.id);
		entry.similarity = match->similarity;

		if ((error = walk_emit(walk, &entry)) != 0)
			goto done;

		/* keep following the old path of the rename */
		error = collect_state(new_states, parent_id,
			match->old_file.path, parent_index);
	} else if (match && match->status == GIT_DELTA_COPIED) {
		entry.type = GIT_PATH_HISTORY_ENTRY_COPIED;
		entry.old_path = match->old_file.path;
		git_oid_cpy(&entry.old_blob_id, &match->old_file.id);
		entry.similarity = match->similarity;

		/*
		 * report the copy, but do not follow the copy source;
		 * this branch of the history ends here
		 */
		error = walk_emit(walk, &entry);
	} else {
		entry.type = GIT_PATH_HISTORY_ENTRY_ADDED;
		error = walk_emit(walk, &entry);
	}

done:
	git_diff_free(diff);
	return error;
}

/* examine one (commit, path) position and enqueue follow-up positions */
static int process_state(path_history_walk *walk, path_history_state *state)
{
	git_commit *commit = NULL, *parent = NULL;
	git_tree *tree = NULL, *parent_tree = NULL;
	git_tree_entry *entry = NULL, *parent_entry = NULL;
	git_vector new_states = GIT_VECTOR_INIT;
	const git_oid *blob_id;
	size_t nparents, max_parents, i;
	int error = 0;

	if ((error = git_commit_lookup(&commit, walk->repo, &state->commit_id)) < 0 ||
	    (error = git_commit_tree(&tree, commit)) < 0)
		goto done;

	error = git_tree_entry_bypath(&entry, tree, state->path);
	if (error == GIT_ENOTFOUND ||
	    (error == 0 && git_tree_entry_type(entry) != GIT_OBJECT_BLOB)) {
		/* the path is not a file here; nothing to report */
		error = 0;
		goto done;
	} else if (error < 0) {
		goto done;
	}

	blob_id = git_tree_entry_id(entry);
	nparents = git_commit_parentcount(commit);

	if (nparents == 0) {
		git_path_history_entry root_entry;

		memset(&root_entry, 0, sizeof(root_entry));
		root_entry.type = GIT_PATH_HISTORY_ENTRY_ADDED;
		git_oid_cpy(&root_entry.commit_id, &state->commit_id);
		root_entry.path = state->path;
		git_oid_cpy(&root_entry.blob_id, blob_id);
		root_entry.parent_index = state->parent_index;

		error = walk_emit(walk, &root_entry);
		goto done;
	}

	max_parents = (walk->opts.strategy == GIT_PATH_HISTORY_STRATEGY_ALL_PARENTS) ?
		nparents : 1;

	if ((error = git_vector_init(&new_states, max_parents, NULL)) < 0)
		goto done;

	for (i = 0; i < max_parents; i++) {
		const git_oid *parent_id;

		git_commit_free(parent);
		git_tree_free(parent_tree);
		git_tree_entry_free(parent_entry);
		parent = NULL, parent_tree = NULL, parent_entry = NULL;

		if ((error = git_commit_parent(&parent, commit, i)) < 0)
			goto done;
		parent_id = git_commit_id(parent);

		if ((error = git_commit_tree(&parent_tree, parent)) < 0)
			goto done;

		error = git_tree_entry_bypath(&parent_entry, parent_tree, state->path);

		if (error == 0 &&
		    git_tree_entry_type(parent_entry) == GIT_OBJECT_BLOB) {
			const git_oid *parent_blob_id = git_tree_entry_id(parent_entry);

			if (git_oid_equal(parent_blob_id, blob_id)) {
				/* unchanged along this edge; follow silently */
				error = collect_state(&new_states, parent_id,
					state->path, i);
				if (error < 0)
					goto done;
			} else {
				git_path_history_entry mod_entry;

				memset(&mod_entry, 0, sizeof(mod_entry));
				mod_entry.type = GIT_PATH_HISTORY_ENTRY_MODIFIED;
				git_oid_cpy(&mod_entry.commit_id, &state->commit_id);
				mod_entry.path = state->path;
				git_oid_cpy(&mod_entry.blob_id, blob_id);
				git_oid_cpy(&mod_entry.old_blob_id, parent_blob_id);
				mod_entry.parent_index = i;

				if ((error = walk_emit(walk, &mod_entry)) != 0)
					goto done;

				error = collect_state(&new_states, parent_id,
					state->path, i);
				if (error < 0)
					goto done;
			}
		} else if (error == GIT_ENOTFOUND) {
			error = process_missing_in_parent(
				walk, tree, parent_tree, parent_id,
				state, blob_id, i, &new_states);
			if (error != 0)
				goto done;
		} else {
			goto done;
		}
	}

	/*
	 * enqueue the new positions in reverse order so that the first
	 * parent's chain is processed first (the stack is LIFO)
	 */
	while (git_vector_length(&new_states) > 0) {
		path_history_state *next = git_vector_last(&new_states);
		git_vector_pop(&new_states);

		if ((error = walk_push(walk, &next->commit_id, next->path,
		                       next->parent_index)) < 0) {
			state_free(next);
			goto done;
		}
		state_free(next);
	}

done:
	{
		path_history_state *leftover;
		size_t j;

		git_vector_foreach(&new_states, j, leftover)
			state_free(leftover);
		git_vector_dispose(&new_states);
	}

	git_commit_free(parent);
	git_tree_free(parent_tree);
	git_tree_entry_free(parent_entry);
	git_tree_entry_free(entry);
	git_tree_free(tree);
	git_commit_free(commit);

	return error;
}

int git_path_history_options_init(
	git_path_history_options *opts,
	unsigned int version)
{
	GIT_INIT_STRUCTURE_FROM_TEMPLATE(
		opts, version, git_path_history_options,
		GIT_PATH_HISTORY_OPTIONS_INIT);
	return 0;
}

int git_path_history_foreach(
	git_repository *repo,
	const git_oid *start_commit,
	const char *path,
	const git_path_history_options *opts,
	git_path_history_cb callback,
	void *payload)
{
	path_history_walk walk;
	path_history_state *state;
	git_commit *commit = NULL;
	git_tree *tree = NULL;
	git_tree_entry *entry = NULL;
	int error = 0;

	GIT_ASSERT_ARG(repo);
	GIT_ASSERT_ARG(start_commit);
	GIT_ASSERT_ARG(path && path[0]);
	GIT_ASSERT_ARG(callback);
	GIT_ERROR_CHECK_VERSION(opts, GIT_PATH_HISTORY_OPTIONS_VERSION,
		"git_path_history_options");

	memset(&walk, 0, sizeof(walk));
	walk.repo = repo;
	walk.callback = callback;
	walk.payload = payload;

	if (opts)
		memcpy(&walk.opts, opts, sizeof(git_path_history_options));
	else if ((error = git_path_history_options_init(
			&walk.opts, GIT_PATH_HISTORY_OPTIONS_VERSION)) < 0)
		return error;

	if (walk.opts.strategy != GIT_PATH_HISTORY_STRATEGY_FIRST_PARENT &&
	    walk.opts.strategy != GIT_PATH_HISTORY_STRATEGY_ALL_PARENTS) {
		git_error_set(GIT_ERROR_INVALID,
			"invalid path history strategy");
		return -1;
	}

	if (walk.opts.find_opts.flags == GIT_DIFF_FIND_BY_CONFIG)
		walk.opts.find_opts.flags = DEFAULT_FIND_FLAGS;

	if ((error = git_vector_init(&walk.stack, 8, NULL)) < 0)
		return error;

	if ((error = git_pool_init(&walk.pool, 1)) < 0)
		goto done;

	/* the starting path must name a file in the starting commit */
	if ((error = git_commit_lookup(&commit, repo, start_commit)) < 0 ||
	    (error = git_commit_tree(&tree, commit)) < 0)
		goto done;

	error = git_tree_entry_bypath(&entry, tree, path);
	if (error == GIT_ENOTFOUND) {
		git_error_set(GIT_ERROR_INVALID,
			"path does not exist in the start commit: %s", path);
		goto done;
	} else if (error < 0) {
		goto done;
	} else if (git_tree_entry_type(entry) != GIT_OBJECT_BLOB) {
		git_error_set(GIT_ERROR_INVALID,
			"path does not name a file in the start commit: %s", path);
		error = -1;
		goto done;
	}

	if ((error = walk_push(&walk, start_commit, path, 0)) < 0)
		goto done;

	while (git_vector_length(&walk.stack) > 0) {
		state = git_vector_last(&walk.stack);
		git_vector_pop(&walk.stack);

		error = process_state(&walk, state);
		state_free(state);

		if (error != 0)
			goto done;
	}

done:
	git_tree_entry_free(entry);
	git_tree_free(tree);
	git_commit_free(commit);
	walk_dispose(&walk);

	return error;
}
