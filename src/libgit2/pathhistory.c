/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "common.h"

#include "git2/pathhistory.h"
#include "git2/commit.h"
#include "git2/diff.h"
#include "git2/errors.h"
#include "git2/repository.h"
#include "git2/tree.h"

#include "hashmap_str.h"
#include "pool.h"
#include "str.h"
#include "vector.h"

#define DEFAULT_RENAME_THRESHOLD 50

typedef struct {
	git_oid commit_id;
	const char *path; /* owned by the walk pool */
} ph_state;

typedef struct {
	git_repository *repo;
	git_pathhistory_options opts;
	git_pathhistory_cb cb;
	void *payload;

	git_vector queue;       /* ph_state *, processed breadth-first */
	size_t head;
	git_hashset_str visited; /* (commit, path) states already queued */
	git_pool pool;          /* owns state paths and visited keys */

	unsigned int emitted;
	bool truncated;
} ph_walk;

static int normalize_options(
	git_pathhistory_options *out,
	const git_pathhistory_options *opts)
{
	if (opts) {
		GIT_ERROR_CHECK_VERSION(opts,
			GIT_PATHHISTORY_OPTIONS_VERSION, "git_pathhistory_options");
		memcpy(out, opts, sizeof(git_pathhistory_options));
	} else {
		git_pathhistory_options default_opts = GIT_PATHHISTORY_OPTIONS_INIT;
		memcpy(out, &default_opts, sizeof(git_pathhistory_options));
	}

	if (out->rename_threshold == 0 || out->rename_threshold > 100)
		out->rename_threshold = DEFAULT_RENAME_THRESHOLD;

	return 0;
}

static int ph_emit(
	ph_walk *w,
	const git_oid *commit_id,
	const git_oid *parent_id,
	unsigned int parent_index,
	git_pathhistory_entry_type_t type,
	const char *path,
	const char *old_path,
	const git_oid *blob_id,
	const git_oid *old_blob_id,
	uint16_t similarity)
{
	git_pathhistory_entry entry;

	memset(&entry, 0, sizeof(entry));
	git_oid_cpy(&entry.commit_id, commit_id);
	if (parent_id)
		git_oid_cpy(&entry.parent_id, parent_id);
	entry.parent_index = parent_index;
	entry.type = type;
	entry.path = path;
	entry.old_path = old_path;
	if (blob_id)
		git_oid_cpy(&entry.blob_id, blob_id);
	if (old_blob_id)
		git_oid_cpy(&entry.old_blob_id, old_blob_id);
	entry.similarity = similarity;

	w->emitted++;
	if (w->opts.max_entries > 0 && w->emitted >= w->opts.max_entries) {
		entry.flags |= GIT_PATHHISTORY_ENTRY_TRUNCATED;
		w->truncated = true;
	}

	return w->cb(&entry, w->payload);
}

static int ph_push(ph_walk *w, const git_oid *commit_id, const char *path)
{
	git_str key = GIT_STR_INIT;
	char hex[GIT_OID_MAX_HEXSIZE + 1];
	const char *pooled_key, *pooled_path;
	ph_state *st;
	int error = 0;

	git_oid_tostr(hex, sizeof(hex), commit_id);

	if ((error = git_str_puts(&key, hex)) < 0 ||
	    (error = git_str_puts(&key, path)) < 0)
		goto done;

	if (git_hashset_str_contains(&w->visited, key.ptr))
		goto done;

	pooled_key = git_pool_strdup(&w->pool, key.ptr);
	GIT_ERROR_CHECK_ALLOC(pooled_key);

	if ((error = git_hashset_str_add(&w->visited, pooled_key)) < 0)
		goto done;

	pooled_path = git_pool_strdup(&w->pool, path);
	GIT_ERROR_CHECK_ALLOC(pooled_path);

	st = git__malloc(sizeof(ph_state));
	GIT_ERROR_CHECK_ALLOC(st);

	git_oid_cpy(&st->commit_id, commit_id);
	st->path = pooled_path;

	if ((error = git_vector_insert(&w->queue, st)) < 0)
		git__free(st);

done:
	git_str_dispose(&key);
	return error;
}

/*
 * Process a single commit -> parent edge for the given path.  Emits at
 * most one entry and queues the follow-up state for the parent when the
 * history of the path continues along this edge.
 */
static int ph_process_edge(
	ph_walk *w,
	git_commit *commit,
	git_tree *commit_tree,
	const git_oid *commit_id,
	const char *path,
	unsigned int parent_index)
{
	git_diff_options dopts = GIT_DIFF_OPTIONS_INIT;
	git_diff_find_options fopts = GIT_DIFF_FIND_OPTIONS_INIT;
	git_commit *parent = NULL;
	git_tree *parent_tree = NULL;
	git_tree_entry *centry = NULL, *pentry = NULL;
	git_diff *diff = NULL;
	const git_diff_delta *delta = NULL, *d;
	const git_oid *parent_id, *blob_id;
	size_t i, ndeltas;
	bool in_parent;
	int error = 0;

	if ((error = git_commit_parent(&parent, commit, parent_index)) < 0)
		goto done;
	parent_id = git_commit_id(parent);

	if ((error = git_commit_tree(&parent_tree, parent)) < 0)
		goto done;

	if (git_tree_entry_bypath(&centry, commit_tree, path) < 0) {
		/* states always refer to existing paths; nothing to do */
		error = 0;
		goto done;
	}
	blob_id = git_tree_entry_id(centry);

	in_parent = (git_tree_entry_bypath(&pentry, parent_tree, path) == 0);

	/* fast path: the path is unchanged along this edge */
	if (in_parent &&
	    git_oid_equal(git_tree_entry_id(pentry), blob_id) &&
	    git_tree_entry_filemode(pentry) == git_tree_entry_filemode(centry)) {
		error = ph_push(w, parent_id, path);
		goto done;
	}

	/*
	 * Slow path: diff the trees and use the similarity machinery to
	 * detect renames and copies involving the path.  Unmodified
	 * entries are included so that copies from unmodified files can
	 * be found; they are removed again by find_similar.
	 */
	dopts.flags = GIT_DIFF_INCLUDE_UNMODIFIED;

	if ((error = git_diff_tree_to_tree(&diff, w->repo,
			parent_tree, commit_tree, &dopts)) < 0)
		goto done;

	fopts.flags = GIT_DIFF_FIND_RENAMES |
		GIT_DIFF_FIND_COPIES |
		GIT_DIFF_FIND_COPIES_FROM_UNMODIFIED |
		GIT_DIFF_FIND_REMOVE_UNMODIFIED;
	fopts.rename_threshold = (uint16_t)w->opts.rename_threshold;
	fopts.copy_threshold = (uint16_t)w->opts.rename_threshold;

	if ((error = git_diff_find_similar(diff, &fopts)) < 0)
		goto done;

	ndeltas = git_diff_num_deltas(diff);
	for (i = 0; i < ndeltas; i++) {
		d = git_diff_get_delta(diff, i);
		if (d->new_file.path && strcmp(d->new_file.path, path) == 0) {
			delta = d;
			break;
		}
	}

	if (!delta) {
		/*
		 * The path is not part of the diff output; fall back to
		 * classifying by its presence in the parent tree.
		 */
		if (in_parent) {
			if ((error = ph_emit(w, commit_id, parent_id, parent_index,
					GIT_PATHHISTORY_MODIFIED, path, NULL,
					blob_id, NULL, 0)) == 0)
				error = ph_push(w, parent_id, path);
		} else {
			error = ph_emit(w, commit_id, parent_id, parent_index,
				GIT_PATHHISTORY_ADDED, path, NULL,
				blob_id, NULL, 0);
		}
		goto done;
	}

	switch (delta->status) {
	case GIT_DELTA_RENAMED:
		if ((error = ph_emit(w, commit_id, parent_id, parent_index,
				GIT_PATHHISTORY_RENAMED, path, delta->old_file.path,
				blob_id, &delta->old_file.id,
				delta->similarity)) == 0)
			/* follow the rename source */
			error = ph_push(w, parent_id, delta->old_file.path);
		break;
	case GIT_DELTA_COPIED:
		/* report the copy, but never follow the copy source */
		if ((error = ph_emit(w, commit_id, parent_id, parent_index,
				GIT_PATHHISTORY_COPIED, path, delta->old_file.path,
				blob_id, &delta->old_file.id,
				delta->similarity)) == 0 && in_parent)
			error = ph_push(w, parent_id, path);
		break;
	case GIT_DELTA_ADDED:
		error = ph_emit(w, commit_id, parent_id, parent_index,
			GIT_PATHHISTORY_ADDED, path, NULL, blob_id, NULL, 0);
		break;
	case GIT_DELTA_UNMODIFIED:
		error = ph_push(w, parent_id, path);
		break;
	default:
		if ((error = ph_emit(w, commit_id, parent_id, parent_index,
				GIT_PATHHISTORY_MODIFIED, path, NULL,
				blob_id, NULL, 0)) == 0)
			error = ph_push(w, parent_id, path);
		break;
	}

done:
	git_diff_free(diff);
	git_tree_entry_free(centry);
	git_tree_entry_free(pentry);
	git_tree_free(parent_tree);
	git_commit_free(parent);
	return error;
}

static int ph_process_state(ph_walk *w, const ph_state *st)
{
	git_commit *commit = NULL;
	git_tree *tree = NULL;
	git_tree_entry *centry = NULL;
	unsigned int parentcount, i, edges;
	int error = 0;

	if ((error = git_commit_lookup(&commit, w->repo, &st->commit_id)) < 0)
		goto done;

	if ((error = git_commit_tree(&tree, commit)) < 0)
		goto done;

	if (git_tree_entry_bypath(&centry, tree, st->path) < 0) {
		/* states always refer to existing paths; nothing to do */
		error = 0;
		goto done;
	}

	parentcount = git_commit_parentcount(commit);
	if (parentcount == 0) {
		error = ph_emit(w, &st->commit_id, NULL, 0,
			GIT_PATHHISTORY_ADDED, st->path, NULL,
			git_tree_entry_id(centry), NULL, 0);
		goto done;
	}

	edges = (w->opts.flags & GIT_PATHHISTORY_ALL_PARENTS) ?
		parentcount : 1;

	for (i = 0; i < edges && !w->truncated; i++) {
		if ((error = ph_process_edge(w, commit, tree,
				&st->commit_id, st->path, i)) < 0)
			goto done;
	}

done:
	git_tree_entry_free(centry);
	git_tree_free(tree);
	git_commit_free(commit);
	return error;
}

static void ph_walk_clear(ph_walk *w)
{
	ph_state *st;
	size_t i;

	git_vector_foreach(&w->queue, i, st)
		git__free(st);
	git_vector_dispose(&w->queue);
	git_hashset_str_dispose(&w->visited);
	git_pool_clear(&w->pool);
}

static int validate_start(
	git_repository *repo,
	const git_oid *commit_id,
	const char *path)
{
	git_commit *commit = NULL;
	git_tree *tree = NULL;
	git_tree_entry *entry = NULL;
	int error;

	if ((error = git_commit_lookup(&commit, repo, commit_id)) < 0)
		goto done;

	if ((error = git_commit_tree(&tree, commit)) < 0)
		goto done;

	if ((error = git_tree_entry_bypath(&entry, tree, path)) < 0) {
		if (error == GIT_ENOTFOUND)
			git_error_set(GIT_ERROR_TREE,
				"path '%s' does not exist in the given commit", path);
		goto done;
	}

done:
	git_tree_entry_free(entry);
	git_tree_free(tree);
	git_commit_free(commit);
	return error;
}

static int pathhistory_walk(
	git_repository *repo,
	const git_oid *commit_id,
	const char *path,
	const git_pathhistory_options *opts,
	git_pathhistory_cb cb,
	void *payload,
	bool *truncated_out)
{
	ph_walk w;
	int error;

	memset(&w, 0, sizeof(w));

	if ((error = normalize_options(&w.opts, opts)) < 0)
		return error;

	w.repo = repo;
	w.cb = cb;
	w.payload = payload;
	git_pool_init(&w.pool, 1);

	if ((error = validate_start(repo, commit_id, path)) < 0)
		goto done;

	if ((error = ph_push(&w, commit_id, path)) < 0)
		goto done;

	while (w.head < w.queue.length && !w.truncated) {
		ph_state *st = git_vector_get(&w.queue, w.head++);

		if ((error = ph_process_state(&w, st)) < 0)
			goto done;
	}

	if (truncated_out)
		*truncated_out = w.truncated;

done:
	ph_walk_clear(&w);
	return error;
}

int git_pathhistory_options_init(
	git_pathhistory_options *opts,
	unsigned int version)
{
	GIT_INIT_STRUCTURE_FROM_TEMPLATE(
		opts, version, git_pathhistory_options,
		GIT_PATHHISTORY_OPTIONS_INIT);
	return 0;
}

int git_pathhistory_foreach(
	git_repository *repo,
	const git_oid *commit_id,
	const char *path,
	const git_pathhistory_options *opts,
	git_pathhistory_cb callback,
	void *payload)
{
	GIT_ASSERT_ARG(repo);
	GIT_ASSERT_ARG(commit_id);
	GIT_ASSERT_ARG(path);
	GIT_ASSERT_ARG(callback);

	return pathhistory_walk(repo, commit_id, path, opts,
		callback, payload, NULL);
}

struct git_pathhistory {
	git_pathhistory_entry *entries;
	size_t count;
	size_t size;
	size_t pos;
	git_pool pool; /* owns the entry strings */
	bool truncated;
};

static int collect_cb(const git_pathhistory_entry *entry, void *payload)
{
	git_pathhistory *ph = payload;
	git_pathhistory_entry *dst;
	size_t new_size;

	if (ph->count == ph->size) {
		git_pathhistory_entry *new_entries;

		new_size = ph->size ? ph->size * 2 : 16;
		GIT_ERROR_CHECK_ALLOC_ADD(&new_size, new_size, 1);

		new_entries = git__reallocarray(ph->entries, new_size,
			sizeof(git_pathhistory_entry));
		GIT_ERROR_CHECK_ALLOC(new_entries);

		ph->entries = new_entries;
		ph->size = new_size;
	}

	dst = &ph->entries[ph->count];
	memcpy(dst, entry, sizeof(git_pathhistory_entry));

	dst->path = git_pool_strdup(&ph->pool, entry->path);
	GIT_ERROR_CHECK_ALLOC(dst->path);

	if (entry->old_path) {
		dst->old_path = git_pool_strdup(&ph->pool, entry->old_path);
		GIT_ERROR_CHECK_ALLOC(dst->old_path);
	}

	ph->count++;
	return 0;
}

int git_pathhistory_new(
	git_pathhistory **out,
	git_repository *repo,
	const git_oid *commit_id,
	const char *path,
	const git_pathhistory_options *opts)
{
	git_pathhistory *ph;
	int error;

	GIT_ASSERT_ARG(out);
	GIT_ASSERT_ARG(repo);
	GIT_ASSERT_ARG(commit_id);
	GIT_ASSERT_ARG(path);

	*out = NULL;

	ph = git__calloc(1, sizeof(git_pathhistory));
	GIT_ERROR_CHECK_ALLOC(ph);

	git_pool_init(&ph->pool, 1);

	if ((error = pathhistory_walk(repo, commit_id, path, opts,
			collect_cb, ph, &ph->truncated)) < 0) {
		git_pathhistory_free(ph);
		return error;
	}

	*out = ph;
	return 0;
}

int git_pathhistory_next(git_pathhistory_entry *out, git_pathhistory *ph)
{
	GIT_ASSERT_ARG(out);
	GIT_ASSERT_ARG(ph);

	if (ph->pos >= ph->count)
		return GIT_ITEROVER;

	memcpy(out, &ph->entries[ph->pos++], sizeof(git_pathhistory_entry));
	return 0;
}

int git_pathhistory_truncated(git_pathhistory *ph)
{
	GIT_ASSERT_ARG(ph);

	return ph->truncated ? 1 : 0;
}

void git_pathhistory_free(git_pathhistory *ph)
{
	if (!ph)
		return;

	git_pool_clear(&ph->pool);
	git__free(ph->entries);
	git__free(ph);
}
