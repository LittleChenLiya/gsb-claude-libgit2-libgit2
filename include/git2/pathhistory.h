/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#ifndef INCLUDE_git_path_history_h__
#define INCLUDE_git_path_history_h__

#include "common.h"
#include "types.h"
#include "oid.h"
#include "diff.h"

/**
 * @file git2/pathhistory.h
 * @brief Git path-following history traversal routines
 * @defgroup git_path_history Git path history traversal routines
 * @ingroup Git
 * @{
 */
GIT_BEGIN_DECL

/**
 * How to traverse commits that have more than one parent.
 */
typedef enum {
	/** Only follow the first parent of merge commits. */
	GIT_PATH_HISTORY_STRATEGY_FIRST_PARENT = 0,

	/**
	 * Follow every parent of merge commits. Each reported entry
	 * records the parent edge it was reached through, and any given
	 * (commit, path) state is visited at most once.
	 */
	GIT_PATH_HISTORY_STRATEGY_ALL_PARENTS = 1
} git_path_history_strategy_t;

/**
 * The kind of change a path history entry describes.
 */
typedef enum {
	/** The path first appears in this commit. */
	GIT_PATH_HISTORY_ENTRY_ADDED = 1,

	/** The path's content changed in this commit. */
	GIT_PATH_HISTORY_ENTRY_MODIFIED = 2,

	/**
	 * The path was renamed in this commit; the walk continues
	 * along `old_path` in the parent commit.
	 */
	GIT_PATH_HISTORY_ENTRY_RENAMED = 3,

	/**
	 * The path was copied from `old_path` in this commit. The walk
	 * does NOT continue along the copy source; this branch of the
	 * history ends here.
	 */
	GIT_PATH_HISTORY_ENTRY_COPIED = 4
} git_path_history_entry_type_t;

/**
 * A single event in the history of a path.
 *
 * The strings in this structure are only valid for the duration of
 * the callback that receives the entry.
 */
typedef struct {
	/** The kind of change this entry describes. */
	git_path_history_entry_type_t type;

	/** The commit in which the change happened. */
	git_oid commit_id;

	/** The path of the file in `commit_id`. */
	const char *path;

	/**
	 * The previous path of the file for RENAMED and COPIED entries;
	 * NULL otherwise.
	 */
	const char *old_path;

	/** The id of the blob at `path` in `commit_id`. */
	git_oid blob_id;

	/**
	 * The id of the blob at the source location in the parent
	 * commit; zeroed for ADDED entries.
	 */
	git_oid old_blob_id;

	/** Similarity score (0-100) for RENAMED and COPIED entries. */
	uint16_t similarity;

	/**
	 * The index of the parent edge (0-based) of `commit_id` through
	 * which this entry was reached. This is only different from 0
	 * with GIT_PATH_HISTORY_STRATEGY_ALL_PARENTS on merge commits.
	 */
	unsigned int parent_index;
} git_path_history_entry;

/**
 * Options for `git_path_history_foreach`.
 */
typedef struct {
	unsigned int version;

	/** How to traverse merge commits. */
	git_path_history_strategy_t strategy;

	/**
	 * Maximum number of entries to report; 0 for no limit. When the
	 * limit is reached, `git_path_history_foreach` stops and returns
	 * 1 to flag that the reported history was truncated.
	 */
	size_t max_entries;

	/**
	 * Options controlling rename and copy detection. If `flags` is
	 * left as GIT_DIFF_FIND_BY_CONFIG, the walk defaults to
	 * GIT_DIFF_FIND_RENAMES | GIT_DIFF_FIND_COPIES |
	 * GIT_DIFF_FIND_COPIES_FROM_UNMODIFIED so that renames are
	 * followed and copies are reported (but not followed).
	 */
	git_diff_find_options find_opts;
} git_path_history_options;

/** Current version of `git_path_history_options`. */
#define GIT_PATH_HISTORY_OPTIONS_VERSION 1

/** Static initializer for `git_path_history_options`. */
#define GIT_PATH_HISTORY_OPTIONS_INIT \
	{ GIT_PATH_HISTORY_OPTIONS_VERSION, \
	  GIT_PATH_HISTORY_STRATEGY_FIRST_PARENT, 0, \
	  GIT_DIFF_FIND_OPTIONS_INIT }

/**
 * Initialize a `git_path_history_options` structure.
 *
 * @param opts the structure to initialize
 * @param version the version of the structure; pass
 *        GIT_PATH_HISTORY_OPTIONS_VERSION
 * @return 0 or an error code
 */
GIT_EXTERN(int) git_path_history_options_init(
	git_path_history_options *opts,
	unsigned int version);

/**
 * Callback invoked for each entry of a path history walk.
 *
 * Returning a non-zero value cancels the walk; all resources of the
 * walk are released and `git_path_history_foreach` returns GIT_EUSER.
 *
 * @param entry the history entry; only valid during the callback
 * @param payload the payload passed to `git_path_history_foreach`
 * @return 0 to continue the walk, non-zero to cancel it
 */
typedef int GIT_CALLBACK(git_path_history_cb)(
	const git_path_history_entry *entry,
	void *payload);

/**
 * Walk the history of a path backwards from a given commit.
 *
 * Starting at `start_commit`, the walk reports each commit in which
 * the file at `path` was added, modified, renamed or copied, and
 * continues into the parent commit(s). Renames are detected with the
 * diff similarity machinery (see `git_diff_find_similar`) and the
 * walk keeps following the old path; copies are reported as such but
 * the copy source is not followed any further.
 *
 * With GIT_PATH_HISTORY_STRATEGY_ALL_PARENTS, every parent of a merge
 * commit is examined; entries carry the parent edge they came from in
 * `parent_index`, and each (commit, path) state is visited only once.
 *
 * @param repo the repository containing the commits
 * @param start_commit the id of the commit to start walking from
 * @param path the path of the file in `start_commit` to follow
 * @param opts options for the walk, or NULL for defaults
 * @param callback invoked for each history entry
 * @param payload passed through to the callback
 * @return 0 if the history was walked to exhaustion, 1 if the walk
 *         stopped because `max_entries` was reached (the reported
 *         history is truncated), GIT_EUSER if the callback cancelled
 *         the walk, or a negative error code
 */
GIT_EXTERN(int) git_path_history_foreach(
	git_repository *repo,
	const git_oid *start_commit,
	const char *path,
	const git_path_history_options *opts,
	git_path_history_cb callback,
	void *payload);

/** @} */
GIT_END_DECL
#endif
