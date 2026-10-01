/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#ifndef INCLUDE_git_traversal_h__
#define INCLUDE_git_traversal_h__

#include "common.h"
#include "types.h"
#include "oid.h"

/**
 * @file git2/traversal.h
 * @brief Read-only commit history traversal reporting
 * @defgroup git_traversal Read-only commit history traversal reporting
 * @ingroup Git
 * @{
 */
GIT_BEGIN_DECL

/**
 * The type of event produced by a commit history traversal.
 */
typedef enum {
	/** A commit was visited by the traversal. */
	GIT_TRAVERSAL_EVENT_COMMIT = 1,

	/**
	 * The traversal could not (or was not allowed to) proceed past
	 * a commit or an edge; see `git_traversal_boundary_reason_t`.
	 */
	GIT_TRAVERSAL_EVENT_BOUNDARY = 2
} git_traversal_event_t;

/**
 * The reason that a traversal stopped at a boundary.
 */
typedef enum {
	/** Not a boundary event. */
	GIT_TRAVERSAL_BOUNDARY_NONE = 0,

	/**
	 * The commit is a member of the caller-provided stop set; the
	 * traversal does not expand past it.
	 */
	GIT_TRAVERSAL_BOUNDARY_STOP = 1,

	/**
	 * The commit object is not present in the local object database.
	 * The traversal never attempts to fetch missing objects.
	 */
	GIT_TRAVERSAL_BOUNDARY_MISSING = 2,

	/**
	 * The commit object exists locally, but could not be read or
	 * parsed (for example, it is corrupt or is not a commit).
	 */
	GIT_TRAVERSAL_BOUNDARY_ERROR = 3
} git_traversal_boundary_reason_t;

/**
 * Callback invoked for each event produced by `git_traversal_report`.
 *
 * For `GIT_TRAVERSAL_EVENT_COMMIT` events, `commit` is the visited
 * commit, `parent` is NULL and `reason` is `GIT_TRAVERSAL_BOUNDARY_NONE`.
 *
 * For `GIT_TRAVERSAL_EVENT_BOUNDARY` events, `commit` is the commit
 * whose history could not be traversed further and `reason` describes
 * why. When the boundary was reached by following a parent edge,
 * `parent` is the target of that edge (and `commit` is its source);
 * when the boundary is a starting point itself, `parent` is NULL.
 *
 * Returning a non-zero value from the callback stops the traversal
 * and makes `git_traversal_report` return that value to the caller.
 *
 * @param event the type of event being reported
 * @param commit the visited commit, or the source of the boundary
 * @param parent the target of the boundary edge, or NULL
 * @param reason the boundary reason, or `GIT_TRAVERSAL_BOUNDARY_NONE`
 * @param payload the payload passed to `git_traversal_report`
 * @return 0 to continue the traversal, non-zero to stop it
 */
typedef int GIT_CALLBACK(git_traversal_cb)(
	git_traversal_event_t event,
	const git_oid *commit,
	const git_oid *parent,
	git_traversal_boundary_reason_t reason,
	void *payload);

/**
 * Traverse the commit graph and report visited commits and boundaries.
 *
 * Starting from each commit in `starts`, the traversal follows all
 * parent edges (including every parent of merge commits) and reports
 * each commit it visits exactly once through the callback.
 *
 * The traversal stops expanding a commit when:
 *
 * - the commit is a member of `stops` (reported as
 *   `GIT_TRAVERSAL_BOUNDARY_STOP`),
 * - the commit object is not present in the local object database
 *   (reported as `GIT_TRAVERSAL_BOUNDARY_MISSING`), or
 * - the commit object cannot be read or parsed (reported as
 *   `GIT_TRAVERSAL_BOUNDARY_ERROR`).
 *
 * Each such condition is reported as a boundary event; boundary edges
 * are reported per edge, so a stop commit that is the parent of
 * several visited commits is reported once per edge.
 *
 * This function is strictly read-only: it never fetches objects and
 * never modifies references, the index or the working directory.
 *
 * @param repo the repository to traverse
 * @param starts the commits to start traversing from
 * @param starts_len the number of commits in `starts`
 * @param stops the commits at which to stop traversing
 * @param stops_len the number of commits in `stops`
 * @param callback the callback invoked for each traversal event
 * @param payload opaque pointer passed to the callback
 * @return 0 on success, the callback's non-zero return value if the
 *         traversal was stopped by the caller, or a negative error code
 */
GIT_EXTERN(int) git_traversal_report(
	git_repository *repo,
	const git_oid *starts,
	size_t starts_len,
	const git_oid *stops,
	size_t stops_len,
	git_traversal_cb callback,
	void *payload);

/** @} */
GIT_END_DECL
#endif
