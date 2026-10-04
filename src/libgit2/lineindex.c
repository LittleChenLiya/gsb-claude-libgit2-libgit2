/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "common.h"

#include "git2/lineindex.h"

#include "diff.h"
#include "diff_file.h"
#include "diff_generate.h"
#include "patch.h"
#include "vector.h"

/* A single line inside a hunk.  Only the line numbers are needed for
 * mapping; the origin is implied by which side of the delta the line
 * exists on. */
typedef struct {
	int old_lineno; /* 1-based line in old file, -1 if absent (addition) */
	int new_lineno; /* 1-based line in new file, -1 if absent (deletion) */
} git_lineindex_line;

typedef struct {
	int old_start;
	int old_lines;
	int new_start;
	int new_lines;
	size_t line_count;
	git_lineindex_line *lines;
} git_lineindex_hunk;

typedef struct {
	git_delta_t status;
	uint32_t flags;
	char *old_path;
	char *new_path;
	int old_lines;
	int new_lines;
	int binary;
	size_t hunk_count;
	git_lineindex_hunk *hunks;
} git_lineindex_delta;

struct git_lineindex {
	git_lineindex_delta *deltas;
	size_t delta_count;
};

/* Count the number of text lines in one side of a delta by loading the
 * content.  This is only used for generated diffs, where the content is
 * available through the repository. */
static int lineindex_count_lines(
	git_diff *diff,
	git_diff_delta *delta,
	bool use_old,
	int *out)
{
	git_diff_file_content fc;
	int error = 0;

	*out = 0;
	memset(&fc, 0, sizeof(fc));

	if ((error = git_diff_file_content__init_from_diff(
			&fc, diff, delta, use_old)) < 0)
		return error;

	if ((fc.flags & GIT_DIFF_FLAG__NO_DATA) == 0) {
		error = git_diff_file_content__load(&fc, &diff->opts);
		if (!error && (fc.flags & GIT_DIFF_FLAG__LOADED) != 0) {
			const char *data = fc.map.data;
			size_t len = fc.map.len, i;
			int lines = 0;

			for (i = 0; i < len; i++)
				if (data[i] == '\n')
					lines++;
			if (len > 0 && data[len - 1] != '\n')
				lines++;
			*out = lines;
		}

		git_diff_file_content__unload(&fc);
	}

	return error;
}

static int lineindex_delta_build(
	git_lineindex_delta *d,
	git_diff *diff,
	size_t delta_idx)
{
	git_diff_delta *delta = git_vector_get(&diff->deltas, delta_idx);
	git_patch *patch = NULL;
	size_t nhunks = 0, h;
	int error = 0;

	d->status = delta->status;
	d->flags = delta->flags;
	d->binary = (delta->flags & GIT_DIFF_FLAG_BINARY) != 0;

	if (delta->old_file.path) {
		d->old_path = git__strdup(delta->old_file.path);
		GIT_ERROR_CHECK_ALLOC(d->old_path);
	}
	if (delta->new_file.path) {
		d->new_path = git__strdup(delta->new_file.path);
		GIT_ERROR_CHECK_ALLOC(d->new_path);
	}

	if ((error = git_patch_from_diff(&patch, diff, delta_idx)) < 0)
		goto done;

	if (patch) {
		nhunks = git_patch_num_hunks(patch);
		d->hunk_count = nhunks;

		d->hunks = git__calloc(nhunks ? nhunks : 1,
			sizeof(git_lineindex_hunk));
		GIT_ERROR_CHECK_ALLOC(d->hunks);

		for (h = 0; h < nhunks; h++) {
			const git_diff_hunk *hunk = NULL;
			size_t nlines = 0, l, stored = 0;

			if ((error = git_patch_get_hunk(&hunk, &nlines,
					patch, h)) < 0)
				goto done;

			d->hunks[h].old_start = hunk->old_start;
			d->hunks[h].old_lines = hunk->old_lines;
			d->hunks[h].new_start = hunk->new_start;
			d->hunks[h].new_lines = hunk->new_lines;

			d->hunks[h].lines = git__calloc(nlines ? nlines : 1,
				sizeof(git_lineindex_line));
			GIT_ERROR_CHECK_ALLOC(d->hunks[h].lines);

			for (l = 0; l < nlines; l++) {
				const git_diff_line *line = NULL;

				if ((error = git_patch_get_line_in_hunk(
						&line, patch, h, l)) < 0)
					goto done;

				/* The no-newline-at-end-of-file marks
				 * duplicate the line they follow; skip them,
				 * the content line has the line number. */
				if (line->origin == GIT_DIFF_LINE_CONTEXT_EOFNL ||
				    line->origin == GIT_DIFF_LINE_ADD_EOFNL ||
				    line->origin == GIT_DIFF_LINE_DEL_EOFNL)
					continue;

				d->hunks[h].lines[stored].old_lineno =
					line->old_lineno;
				d->hunks[h].lines[stored].new_lineno =
					line->new_lineno;
				stored++;
			}

			d->hunks[h].line_count = stored;
		}
	} else {
		/* unmodified delta: no hunks */
		d->hunks = git__calloc(1, sizeof(git_lineindex_hunk));
		GIT_ERROR_CHECK_ALLOC(d->hunks);
	}

	/* Total line counts.  For generated diffs we load the file content;
	 * for parsed diffs (from a patch buffer) there is no content, so
	 * fall back to the extent of the hunk data. */
	if (diff->type == GIT_DIFF_TYPE_GENERATED) {
		if (!d->binary) {
			if ((error = lineindex_count_lines(diff, delta,
					true, &d->old_lines)) < 0 ||
			    (error = lineindex_count_lines(diff, delta,
					false, &d->new_lines)) < 0)
				goto done;

			if ((delta->old_file.flags & GIT_DIFF_FLAG_BINARY) ||
			    (delta->new_file.flags & GIT_DIFF_FLAG_BINARY)) {
				d->binary = 1;
				d->old_lines = 0;
				d->new_lines = 0;
			}
		}
	} else {
		for (h = 0; h < nhunks; h++) {
			int old_end = d->hunks[h].old_start +
				d->hunks[h].old_lines - 1;
			int new_end = d->hunks[h].new_start +
				d->hunks[h].new_lines - 1;
			if (old_end > d->old_lines)
				d->old_lines = old_end;
			if (new_end > d->new_lines)
				d->new_lines = new_end;
		}
	}

done:
	git_patch_free(patch);
	return error;
}

int git_lineindex_new(git_lineindex **out, git_diff *diff)
{
	git_lineindex *idx;
	size_t i;
	int error;

	GIT_ASSERT_ARG(out);
	GIT_ASSERT_ARG(diff);

	idx = git__calloc(1, sizeof(git_lineindex));
	GIT_ERROR_CHECK_ALLOC(idx);

	idx->delta_count = git_diff_num_deltas(diff);
	idx->deltas = git__calloc(
		idx->delta_count ? idx->delta_count : 1,
		sizeof(git_lineindex_delta));
	GIT_ERROR_CHECK_ALLOC(idx->deltas);

	for (i = 0; i < idx->delta_count; i++) {
		if ((error = lineindex_delta_build(
				&idx->deltas[i], diff, i)) < 0)
			goto fail;
	}

	*out = idx;
	return 0;

fail:
	git_lineindex_free(idx);
	return error;
}

void git_lineindex_free(git_lineindex *idx)
{
	size_t i, j;

	if (!idx)
		return;

	for (i = 0; i < idx->delta_count; i++) {
		git_lineindex_delta *d = &idx->deltas[i];
		git__free(d->old_path);
		git__free(d->new_path);
		for (j = 0; j < d->hunk_count; j++)
			git__free(d->hunks[j].lines);
		git__free(d->hunks);
	}

	git__free(idx->deltas);
	git__free(idx);
}

static git_lineindex_delta *lineindex_delta(
	git_lineindex *idx,
	size_t delta_idx)
{
	if (delta_idx >= idx->delta_count)
		return NULL;
	return &idx->deltas[delta_idx];
}

static const git_lineindex_line *lineindex_hunk_find_old(
	git_lineindex_hunk *h,
	int old_lineno)
{
	size_t i;

	for (i = 0; i < h->line_count; i++)
		if (h->lines[i].old_lineno == old_lineno)
			return &h->lines[i];
	return NULL;
}

static const git_lineindex_line *lineindex_hunk_find_new(
	git_lineindex_hunk *h,
	int new_lineno)
{
	size_t i;

	for (i = 0; i < h->line_count; i++)
		if (h->lines[i].new_lineno == new_lineno)
			return &h->lines[i];
	return NULL;
}

/* For a deleted old line, find the new line numbers that bracket the
 * deletion point: the closest unchanged (context) new line before and
 * after it.  Additions are skipped because they are themselves new. */
static void lineindex_deleted_brackets(
	git_lineindex_hunk *h,
	size_t li,
	int *before,
	int *after,
	int new_total)
{
	size_t i;

	*before = 0;
	*after = 0;

	for (i = li; i > 0; i--) {
		if (h->lines[i - 1].old_lineno >= 0 &&
		    h->lines[i - 1].new_lineno >= 0) {
			*before = h->lines[i - 1].new_lineno;
			break;
		}
	}
	if (*before == 0) {
		/* A zero-length new range is a deletion point; the line
		 * before it is new_start, not new_start - 1. */
		*before = h->new_lines == 0 ?
			h->new_start : h->new_start - 1;
		if (*before < 0)
			*before = 0;
	}

	for (i = li + 1; i < h->line_count; i++) {
		if (h->lines[i].old_lineno >= 0 &&
		    h->lines[i].new_lineno >= 0) {
			*after = h->lines[i].new_lineno;
			break;
		}
	}
	if (*after == 0) {
		*after = h->new_lines == 0 ?
			h->new_start + 1 : h->new_start + h->new_lines;
		if (*after > new_total)
			*after = 0;
	}
}

/* For an added new line, find the old line numbers that bracket the
 * addition point. */
static void lineindex_added_brackets(
	git_lineindex_hunk *h,
	size_t li,
	int *before,
	int *after,
	int old_total)
{
	size_t i;

	*before = 0;
	*after = 0;

	for (i = li; i > 0; i--) {
		if (h->lines[i - 1].old_lineno >= 0 &&
		    h->lines[i - 1].new_lineno >= 0) {
			*before = h->lines[i - 1].old_lineno;
			break;
		}
	}
	if (*before == 0) {
		/* A zero-length old range is an insertion point; the line
		 * before it is old_start, not old_start - 1. */
		*before = h->old_lines == 0 ?
			h->old_start : h->old_start - 1;
		if (*before < 0)
			*before = 0;
	}

	for (i = li + 1; i < h->line_count; i++) {
		if (h->lines[i].old_lineno >= 0 &&
		    h->lines[i].new_lineno >= 0) {
			*after = h->lines[i].old_lineno;
			break;
		}
	}
	if (*after == 0) {
		*after = h->old_lines == 0 ?
			h->old_start + 1 : h->old_start + h->old_lines;
		if (*after > old_total)
			*after = 0;
	}
}

size_t git_lineindex_delta_count(const git_lineindex *idx)
{
	GIT_ASSERT_ARG(idx);
	return idx->delta_count;
}

int git_lineindex_get_delta_info(
	git_lineindex *idx,
	size_t delta_idx,
	git_lineindex_delta_info *out)
{
	git_lineindex_delta *d;

	GIT_ASSERT_ARG(idx);
	GIT_ASSERT_ARG(out);

	if (!(d = lineindex_delta(idx, delta_idx)))
		return GIT_ENOTFOUND;

	memset(out, 0, sizeof(*out));
	out->status = d->status;
	out->flags = d->flags;
	out->old_path = d->old_path;
	out->new_path = d->new_path;
	out->old_lines = d->old_lines;
	out->new_lines = d->new_lines;
	out->binary = d->binary;
	return 0;
}

int git_lineindex_lookup_old(
	git_lineindex *idx,
	size_t delta_idx,
	int old_lineno,
	git_lineindex_location *out)
{
	git_lineindex_delta *d;
	size_t i;

	GIT_ASSERT_ARG(idx);
	GIT_ASSERT_ARG(out);

	memset(out, 0, sizeof(*out));

	if (!(d = lineindex_delta(idx, delta_idx)))
		return GIT_ENOTFOUND;

	if (d->binary) {
		out->status = GIT_LINEINDEX_UNMAPPABLE;
		return 0;
	}

	if (old_lineno < 1 || old_lineno > d->old_lines) {
		out->status = GIT_LINEINDEX_NONE;
		out->old_lineno = old_lineno;
		return 0;
	}

	for (i = 0; i < d->hunk_count; i++) {
		git_lineindex_hunk *h = &d->hunks[i];

		if (old_lineno >= h->old_start &&
		    old_lineno < h->old_start + h->old_lines) {
			const git_lineindex_line *ln =
				lineindex_hunk_find_old(h, old_lineno);

			if (ln) {
				out->old_lineno = old_lineno;
				out->hunk_old_start = h->old_start;
				out->hunk_old_lines = h->old_lines;
				out->hunk_new_start = h->new_start;
				out->hunk_new_lines = h->new_lines;

				if (ln->new_lineno >= 0) {
					out->status = GIT_LINEINDEX_CONTEXT;
					out->new_lineno = ln->new_lineno;
				} else {
					size_t li = ln - h->lines;
					out->status = GIT_LINEINDEX_DELETED;
					lineindex_deleted_brackets(h, li,
						&out->before, &out->after,
						d->new_lines);
				}
				return 0;
			}
		}
	}

	/* Unchanged line outside of any hunk: map it through the cumulative
	 * line shift of the hunks that precede it. */
	out->status = GIT_LINEINDEX_CONTEXT;
	out->old_lineno = old_lineno;
	{
		int offset = 0;
		for (i = 0; i < d->hunk_count; i++) {
			git_lineindex_hunk *h = &d->hunks[i];
			/* A zero-length old range is an insertion point;
			 * it precedes only lines after it. */
			if (h->old_lines == 0 ?
			    h->old_start < old_lineno :
			    h->old_start + h->old_lines <= old_lineno)
				offset += h->new_lines - h->old_lines;
		}
		out->new_lineno = old_lineno + offset;
	}
	return 0;
}

int git_lineindex_lookup_new(
	git_lineindex *idx,
	size_t delta_idx,
	int new_lineno,
	git_lineindex_location *out)
{
	git_lineindex_delta *d;
	size_t i;

	GIT_ASSERT_ARG(idx);
	GIT_ASSERT_ARG(out);

	memset(out, 0, sizeof(*out));

	if (!(d = lineindex_delta(idx, delta_idx)))
		return GIT_ENOTFOUND;

	if (d->binary) {
		out->status = GIT_LINEINDEX_UNMAPPABLE;
		return 0;
	}

	if (new_lineno < 1 || new_lineno > d->new_lines) {
		out->status = GIT_LINEINDEX_NONE;
		out->new_lineno = new_lineno;
		return 0;
	}

	for (i = 0; i < d->hunk_count; i++) {
		git_lineindex_hunk *h = &d->hunks[i];

		if (new_lineno >= h->new_start &&
		    new_lineno < h->new_start + h->new_lines) {
			const git_lineindex_line *ln =
				lineindex_hunk_find_new(h, new_lineno);

			if (ln) {
				out->new_lineno = new_lineno;
				out->hunk_old_start = h->old_start;
				out->hunk_old_lines = h->old_lines;
				out->hunk_new_start = h->new_start;
				out->hunk_new_lines = h->new_lines;

				if (ln->old_lineno >= 0) {
					out->status = GIT_LINEINDEX_CONTEXT;
					out->old_lineno = ln->old_lineno;
				} else {
					size_t li = ln - h->lines;
					out->status = GIT_LINEINDEX_ADDED;
					lineindex_added_brackets(h, li,
						&out->before, &out->after,
						d->old_lines);
				}
				return 0;
			}
		}
	}

	/* Unchanged line outside of any hunk: map it through the cumulative
	 * line shift of the hunks that precede it. */
	out->status = GIT_LINEINDEX_CONTEXT;
	out->new_lineno = new_lineno;
	{
		int offset = 0;
		for (i = 0; i < d->hunk_count; i++) {
			git_lineindex_hunk *h = &d->hunks[i];
			/* A zero-length new range is a deletion point;
			 * it precedes only lines after it. */
			if (h->new_lines == 0 ?
			    h->new_start < new_lineno :
			    h->new_start + h->new_lines <= new_lineno)
				offset += h->old_lines - h->new_lines;
		}
		out->old_lineno = new_lineno + offset;
	}
	return 0;
}
