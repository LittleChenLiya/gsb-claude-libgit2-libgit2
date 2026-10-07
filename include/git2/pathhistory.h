/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#ifndef INCLUDE_git_pathhistory_h__
#define INCLUDE_git_pathhistory_h__

#include "common.h"
#include "types.h"
#include "oid.h"

/**
 * @file git2/pathhistory.h
 * @brief Git path-following commit history traversal
 * @defgroup git_pathhistory Git path-following commit history traversal
 * @ingroup Git
 * @{
 */
GIT_BEGIN_DECL

/**
 * Flags to control the path history traversal.
 */
typedef enum {
	/**
	 * At merge commits, only follow the first parent.  This is the
	 * default behavior.
	 */
	GIT_PATHHISTORY_FIRST_PARENT = 0,

	/**
	 * At merge commits, follow all parents.  Each reported entry
	 * records the parent edge it was found through, and any given
	 * (commit, path) state is only visited once.
	 */
	GIT_PATHHISTORY_ALL_PARENTS = (1u << 0)
} git_pathhistory_flag_t;

/**
 * Options for the path history traversal.
 */
typedef struct {
	unsigned int version;

	/**
	 * Combination of `git_pathhistory_flag_t` values.
	 */
	uint32_t flags;

	/**
	 * Maximum number of entries to report; 0 means unlimited.
	 *
	 * When the limit is reached, the traversal stops and the last
	 * reported entry is flagged with `GIT_PATHHISTORY_ENTRY_TRUNCATED`.
	 * When using the `git_pathhistory` iterator,
	 * `git_pathhistory_truncated` also reports whether the walk was
	 * cut short by this limit.
	 */
	unsigned int max_entries;

	/**
	 * Similarity threshold (in percent, 0-100) above which a
	 * delete/add pair is considered a rename or a copy.  This is
	 * equivalent to the `-M`/`-C` options of git-diff.  Pass 0 to
	 * use the default of 50.
	 */
	uint16_t rename_threshold;
} git_pathhistory_options;

/** Current version for the `git_pathhistory_options` structure */
#define GIT_PATHHISTORY_OPTIONS_VERSION 1

/** Static constructor for `git_pathhistory_options` */
#define GIT_PATHHISTORY_OPTIONS_INIT {GIT_PATHHISTORY_OPTIONS_VERSION}

/**
 * Initialize git_pathhistory_options structure
 *
 * Initializes a `git_pathhistory_options` with default values. Equivalent
 * to creating an instance with GIT_PATHHISTORY_OPTIONS_INIT.
 *
 * @param opts The `git_pathhistory_options` struct to initialize.
 * @param version The struct version; pass `GIT_PATHHISTORY_OPTIONS_VERSION`.
 * @return Zero on success; -1 on failure.
 */
GIT_EXTERN(int) git_pathhistory_options_init(
	git_pathhistory_options *opts,
	unsigned int version);

/**
 * The kind of change a path history entry describes.
 */
typedef enum {
	/** The path was added in this commit (history does not continue). */
	GIT_PATHHISTORY_ADDED = 1,

	/** The path content (or mode) was modified in this commit. */
	GIT_PATHHISTORY_MODIFIED = 2,

	/**
	 * The path was renamed in this commit; the history continues
	 * along `old_path` in the parent commit.
	 */
	GIT_PATHHISTORY_RENAMED = 3,

	/**
	 * The path was created as a copy of `old_path` in this commit.
	 * The history does *not* continue along the copy source.
	 */
	GIT_PATHHISTORY_COPIED = 4
} git_pathhistory_entry_type_t;

/**
 * Flags that may be set on a path history entry.
 */
typedef enum {
	/**
	 * This is the last entry because the configured
	 * `max_entries` limit was reached; more history may exist.
	 */
	GIT_PATHHISTORY_ENTRY_TRUNCATED = (1u << 0)
} git_pathhistory_entry_flag_t;

/**
 * A single path history entry.
 *
 * The strings pointed to by this structure are owned by the
 * `git_pathhistory` iterator (and stay valid until it is freed),
 * or are only valid for the duration of the callback when using
 * `git_pathhistory_foreach`.
 */
typedef struct {
	/** The commit in which the change happened. */
	git_oid commit_id;

	/**
	 * The parent commit this change was computed against.  This
	 * identifies the "source edge" of the entry; it is zeroed when
	 * the change happened in a root commit.
	 */
	git_oid parent_id;

	/** The index of `parent_id` within the parents of `commit_id`. */
	unsigned int parent_index;

	/** The kind of change. */
	git_pathhistory_entry_type_t type;

	/** Combination of `git_pathhistory_entry_flag_t` values. */
	uint32_t flags;

	/** The path of the file in `commit_id`. */
	const char *path;

	/**
	 * The source path in `parent_id` for `GIT_PATHHISTORY_RENAMED`
	 * and `GIT_PATHHISTORY_COPIED` entries; NULL otherwise.
	 */
	const char *old_path;

	/** The id of the blob at `path` in `commit_id`. */
	git_oid blob_id;

	/**
	 * The id of the blob at `old_path` in `parent_id` for renamed
	 * and copied entries; zeroed otherwise.
	 */
	git_oid old_blob_id;

	/** Similarity score for renamed and copied entries; 0 otherwise. */
	uint16_t similarity;
} git_pathhistory_entry;

/**
 * Path history iterator object.
 */
typedef struct git_pathhistory git_pathhistory;

/**
 * Create a new path history iterator.
 *
 * The traversal starts at the given commit and follows the history of
 * the file at `path` backwards.  When a rename is detected (using the
 * diff similarity machinery), the traversal continues along the
 * previous path.  Copies are reported but never followed.
 *
 * @param out The newly allocated iterator; free with
 * `git_pathhistory_free`.
 * @param repo The repository to search in.
 * @param commit_id The commit to start the traversal from.
 * @param path The path of the file to follow; must exist in the
 * given commit.
 * @param opts Options for the traversal, or NULL for defaults.
 * @return Zero on success, GIT_ENOTFOUND if the path does not exist
 * in the given commit, or an error code.
 */
GIT_EXTERN(int) git_pathhistory_new(
	git_pathhistory **out,
	git_repository *repo,
	const git_oid *commit_id,
	const char *path,
	const git_pathhistory_options *opts);

/**
 * Get the next path history entry.
 *
 * @param out The entry to fill in; the pointers in it stay valid
 * until the iterator is freed.
 * @param ph The iterator.
 * @return Zero on success, GIT_ITEROVER when there are no more
 * entries, or an error code.
 */
GIT_EXTERN(int) git_pathhistory_next(
	git_pathhistory_entry *out,
	git_pathhistory *ph);

/**
 * Check whether the traversal was cut short by the configured
 * `max_entries` limit.
 *
 * @param ph The iterator.
 * @return 1 if the limit was reached and more history may exist,
 * 0 if the full history was reported.
 */
GIT_EXTERN(int) git_pathhistory_truncated(git_pathhistory *ph);

/**
 * Free a path history iterator.
 *
 * @param ph The iterator to free, or NULL.
 */
GIT_EXTERN(void) git_pathhistory_free(git_pathhistory *ph);

/**
 * Callback for `git_pathhistory_foreach`.
 *
 * Returning a non-zero value stops the traversal; that value is
 * returned to the caller of `git_pathhistory_foreach`.  All resources
 * held by the traversal are released before it returns.
 */
typedef int GIT_CALLBACK(git_pathhistory_cb)(
	const git_pathhistory_entry *entry,
	void *payload);

/**
 * Walk the history of a path, invoking a callback for each entry.
 *
 * This behaves like `git_pathhistory_new`/`git_pathhistory_next`,
 * but reports entries through a callback and allows the walk to be
 * canceled by returning a non-zero value from the callback.
 *
 * @param repo The repository to search in.
 * @param commit_id The commit to start the traversal from.
 * @param path The path of the file to follow; must exist in the
 * given commit.
 * @param opts Options for the traversal, or NULL for defaults.
 * @param callback The callback to invoke for each entry.
 * @param payload Opaque pointer passed to the callback.
 * @return Zero on success, GIT_ENOTFOUND if the path does not exist
 * in the given commit, the non-zero value returned by the callback
 * when the walk was canceled, or an error code.
 */
GIT_EXTERN(int) git_pathhistory_foreach(
	git_repository *repo,
	const git_oid *commit_id,
	const char *path,
	const git_pathhistory_options *opts,
	git_pathhistory_cb callback,
	void *payload);

/** @} */
GIT_END_DECL
#endif
