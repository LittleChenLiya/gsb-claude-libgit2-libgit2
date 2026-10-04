/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#ifndef INCLUDE_git_lineindex_h__
#define INCLUDE_git_lineindex_h__

#include "common.h"
#include "types.h"
#include "diff.h"

/**
 * @file git2/lineindex.h
 * @brief Line number index for diff results
 * @ingroup Git
 * @{
 *
 * A `git_lineindex` maps the line numbers of the old version of a file in
 * a diff to the line numbers of the new version (and vice versa).  It is
 * meant for offline migration of line-anchored comments: given a comment on
 * old file line `N`, find where that line ended up in the new file.
 *
 * The index is built from a `git_diff` and copies all the information it
 * needs (delta metadata, hunk headers and per-line records).  Once built it
 * does not reference the diff any more, so the diff may be freed and the
 * index can still be used.
 *
 * The index only uses the structured hunk/line data that libgit2 produced
 * for the diff.  It never guesses mappings by fuzzy-searching file
 * contents: a line is reported as mapped only when the diff data says it is
 * unchanged, as deleted when the diff data says it was removed, and as
 * added when the diff data says it was inserted.  Binary files are reported
 * as unmappable.
 */
GIT_BEGIN_DECL

/** The line index object. */
typedef struct git_lineindex git_lineindex;

/**
 * Status of a single line as reported by the line index.
 */
typedef enum {
	/**
	 * The file is binary; no text line mapping is possible.
	 */
	GIT_LINEINDEX_UNMAPPABLE = -1,

	/**
	 * The line number does not refer to an existing line on the
	 * queried side (the number is out of range, or that side of the
	 * delta does not exist, e.g. querying the old side of an added
	 * file).
	 */
	GIT_LINEINDEX_NONE = 0,

	/**
	 * The line is unchanged between the two versions and maps to
	 * exactly one line on the other side.
	 */
	GIT_LINEINDEX_CONTEXT,

	/**
	 * The line exists only in the old version; it was deleted in the
	 * new version.  `new_lineno` is 0 and the `before`/`after` fields
	 * describe where the deletion happened in the new file.
	 */
	GIT_LINEINDEX_DELETED,

	/**
	 * The line exists only in the new version; it was added from the
	 * old version.  `old_lineno` is 0 and the `before`/`after` fields
	 * describe where the addition happened in the old file.
	 */
	GIT_LINEINDEX_ADDED
} git_lineindex_status_t;

/**
 * Location of a line in the other version of a delta.
 */
typedef struct {
	git_lineindex_status_t status;

	/** 1-based line number in the old file, 0 if there is no old line */
	int old_lineno;

	/** 1-based line number in the new file, 0 if there is no new line */
	int new_lineno;

	/**
	 * The hunk that contains the line.  For lines that are not part of
	 * a hunk (unchanged lines between hunks) these are all 0.
	 */
	int hunk_old_start;
	int hunk_old_lines;
	int hunk_new_start;
	int hunk_new_lines;

	/**
	 * Bracketing line number on the opposite side of the change.
	 *
	 * For a `GIT_LINEINDEX_DELETED` result (looking up an old line),
	 * `before` is the new line number of the last unchanged line
	 * before the deletion point and `after` is the new line number of
	 * the first unchanged line after it.  For a `GIT_LINEINDEX_ADDED`
	 * result (looking up a new line), they are the corresponding old
	 * line numbers.  A value of 0 means the change is at the very
	 * beginning (`before`) or very end (`after`) of the file.  For a
	 * `GIT_LINEINDEX_CONTEXT` or `GIT_LINEINDEX_NONE` result these
	 * are 0.
	 */
	int before;
	int after;
} git_lineindex_location;

/**
 * Information about one delta in a line index.
 */
typedef struct {
	/** The delta status (GIT_DELTA_MODIFIED, ...) */
	git_delta_t status;

	/** The delta flags (see `git_diff_flag_t`) */
	uint32_t flags;

	/** Path of the old side (may be NULL for added files) */
	const char *old_path;

	/** Path of the new side (may be NULL for deleted files) */
	const char *new_path;

	/** Total number of lines in the old file (0 if absent or binary) */
	int old_lines;

	/** Total number of lines in the new file (0 if absent or binary) */
	int new_lines;

	/** Non-zero if the delta is a binary file */
	int binary;
} git_lineindex_delta_info;

/**
 * Create a line index from a diff.
 *
 * The index copies the delta metadata, hunk headers and per-line records
 * of the diff, so the returned object is independent of the diff: freeing
 * the diff does not invalidate the index.
 *
 * @param[out] out The newly created line index.
 * @param diff The diff to index.  It does not need to be sorted or
 *             otherwise prepared beyond being generated.
 * @return 0 on success, non-zero on failure.
 */
GIT_EXTERN(int) git_lineindex_new(git_lineindex **out, git_diff *diff);

/**
 * Free a line index.
 *
 * The index is freed independently of the diff it was created from; the
 * diff may be freed before or after the index.
 *
 * @param idx The line index to free.
 */
GIT_EXTERN(void) git_lineindex_free(git_lineindex *idx);

/**
 * Return the number of deltas in a line index.
 *
 * @param idx The line index.
 * @return The number of deltas.
 */
GIT_EXTERN(size_t) git_lineindex_delta_count(const git_lineindex *idx);

/**
 * Get information about one delta in a line index.
 *
 * @param idx The line index.
 * @param delta_idx The index of the delta.
 * @param[out] out The delta information to fill in.
 * @return 0 on success, GIT_ENOTFOUND if `delta_idx` is out of range.
 */
GIT_EXTERN(int) git_lineindex_get_delta_info(
	git_lineindex *idx,
	size_t delta_idx,
	git_lineindex_delta_info *out);

/**
 * Map an old file line number to the new file.
 *
 * @param idx The line index.
 * @param delta_idx The index of the delta.
 * @param old_lineno The 1-based line number in the old file.
 * @param[out] out The location of the line in the new file.
 * @return 0 on success, GIT_ENOTFOUND if `delta_idx` is out of range.
 */
GIT_EXTERN(int) git_lineindex_lookup_old(
	git_lineindex *idx,
	size_t delta_idx,
	int old_lineno,
	git_lineindex_location *out);

/**
 * Map a new file line number to the old file.
 *
 * @param idx The line index.
 * @param delta_idx The index of the delta.
 * @param new_lineno The 1-based line number in the new file.
 * @param[out] out The location of the line in the old file.
 * @return 0 on success, GIT_ENOTFOUND if `delta_idx` is out of range.
 */
GIT_EXTERN(int) git_lineindex_lookup_new(
	git_lineindex *idx,
	size_t delta_idx,
	int new_lineno,
	git_lineindex_location *out);

/** @} */
GIT_END_DECL

#endif
