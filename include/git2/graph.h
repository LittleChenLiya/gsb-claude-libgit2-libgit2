/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#ifndef INCLUDE_git_graph_h__
#define INCLUDE_git_graph_h__

#include "common.h"
#include "types.h"
#include "oid.h"

/**
 * @file git2/graph.h
 * @brief Graph traversal routines
 * @defgroup git_revwalk Git graph traversal routines
 * @ingroup Git
 * @{
 */
GIT_BEGIN_DECL

/**
 * Count the number of unique commits between two commit objects
 *
 * There is no need for branches containing the commits to have any
 * upstream relationship, but it helps to think of one as a branch and
 * the other as its upstream, the `ahead` and `behind` values will be
 * what git would report for the branches.
 *
 * @param[out] ahead number of unique from commits in `upstream`
 * @param[out] behind number of unique from commits in `local`
 * @param repo the repository where the commits exist
 * @param local the commit for local
 * @param upstream the commit for upstream
 * @return 0 or an error code.
 */
GIT_EXTERN(int) git_graph_ahead_behind(
	size_t *ahead,
	size_t *behind,
	git_repository *repo,
	const git_oid *local,
	const git_oid *upstream);


/**
 * Determine if a commit is the descendant of another commit.
 *
 * Note that a commit is not considered a descendant of itself, in contrast
 * to `git merge-base --is-ancestor`.
 *
 * @param repo the repository where the commits exist
 * @param commit a previously loaded commit
 * @param ancestor a potential ancestor commit
 * @return 1 if the given commit is a descendant of the potential ancestor,
 * 0 if not, error code otherwise.
 */
GIT_EXTERN(int) git_graph_descendant_of(
	git_repository *repo,
	const git_oid *commit,
	const git_oid *ancestor);

/**
 * Determine if a commit is reachable from any of a list of commits by
 * following parent edges.
 *
 * @param repo the repository where the commits exist
 * @param commit a previously loaded commit
 * @param descendant_array oids of the commits
 * @param length the number of commits in the provided `descendant_array`
 * @return 1 if the given commit is an ancestor of any of the given potential
 * descendants, 0 if not, error code otherwise.
 */
GIT_EXTERN(int) git_graph_reachable_from_any(
	git_repository *repo,
	const git_oid *commit,
	const git_oid descendant_array[],
	size_t length);

/**
 * The reason why a commit graph traversal could not continue past a
 * given parent edge.
 */
typedef enum {
	/** The commit on the edge is part of the caller-provided stop set. */
	GIT_GRAPH_BOUNDARY_STOP = 1,

	/** The parent commit object does not exist in the local object store. */
	GIT_GRAPH_BOUNDARY_MISSING,

	/** The parent commit object exists but could not be read or parsed. */
	GIT_GRAPH_BOUNDARY_ERROR
} git_graph_boundary_reason;

/**
 * A parent edge at which a commit graph traversal stopped.
 *
 * The edge goes from the commit `from` (which was visited) to its
 * parent `to` (which was not visited). When a starting commit is
 * itself part of the stop set, `from` and `to` are identical.
 */
typedef struct {
	git_oid from;
	git_oid to;
	git_graph_boundary_reason reason;
} git_graph_boundary_edge;

/**
 * The result of a read-only commit graph traversal.
 *
 * `visited` contains the commits that were reached and expanded during
 * the traversal; `edges` contains the boundary edges at which the
 * traversal stopped, along with the reason for each stop.
 *
 * Free with `git_graph_report_free`.
 */
typedef struct {
	git_oid *visited;
	size_t visited_count;
	git_graph_boundary_edge *edges;
	size_t edge_count;
} git_graph_report;

/**
 * Traverse the commit graph from a set of starting commits, following
 * all parent edges, and report which commits were visited and where
 * the traversal stopped.
 *
 * The traversal is strictly read-only: it only reads objects from the
 * local object store and never fetches missing objects, nor modifies
 * references, the index or the working directory.
 *
 * Commits in the `stops` set are never visited; instead, each edge
 * leading to them is reported as a boundary edge with reason
 * `GIT_GRAPH_BOUNDARY_STOP`. A starting commit that is itself in the
 * stop set is reported as a boundary edge from itself to itself.
 *
 * When a parent commit is not present in the local object store, the
 * edge to it is reported with `GIT_GRAPH_BOUNDARY_MISSING`; when the
 * parent object exists but cannot be read or parsed (for example a
 * corrupt object), the edge is reported with `GIT_GRAPH_BOUNDARY_ERROR`.
 * In both cases the traversal continues with the remaining parents.
 *
 * All starting commits must exist and be readable commits; if any
 * starting commit cannot be loaded, an error is returned and no report
 * is produced.
 *
 * @param[out] out the resulting traversal report; free with
 * `git_graph_report_free`
 * @param repo the repository where the commits exist
 * @param starts the commits to start traversing from
 * @param starts_len the number of commits in `starts`
 * @param stops the commits at which to stop traversing
 * @param stops_len the number of commits in `stops`
 * @return 0 or an error code.
 */
GIT_EXTERN(int) git_graph_traverse(
	git_graph_report **out,
	git_repository *repo,
	const git_oid *starts,
	size_t starts_len,
	const git_oid *stops,
	size_t stops_len);

/**
 * Free a commit graph traversal report.
 *
 * @param report the report to free, or NULL
 */
GIT_EXTERN(void) git_graph_report_free(git_graph_report *report);

/** @} */
GIT_END_DECL

#endif
