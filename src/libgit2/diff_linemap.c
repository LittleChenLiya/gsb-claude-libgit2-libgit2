/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "common.h"

#include "diff.h"
#include "patch.h"
#include "array.h"

/*
 * A git_diff_linemap translates line numbers between the old and the
 * new version of a single delta of a diff.
 *
 * The index is built from the delta's hunks and lines only; it never
 * looks at (or guesses from) the actual file contents.  All data that
 * is needed after construction is copied, so the index stays valid
 * once the originating git_diff has been freed.
 *
 * Two sorted tables are kept:
 *
 * - old_entries: one entry per old-file line covered by a hunk, ie
 *   context lines (mapped exactly) and deletions.  Sorted by
 *   old_lineno.
 * - new_entries: one entry per new-file line covered by a hunk, ie
 *   context lines and additions.  Sorted by new_lineno.
 *
 * Lines that are not covered by any hunk are unchanged and map with
 * the cumulative line offset introduced by the preceding hunks.
 */

typedef struct {
	int old_start;
	int old_lines;
	int new_start;
	int new_lines;
} linemap_hunk;

typedef struct {
	int old_lineno;   /* 1-based old line number */
	int new_lineno;   /* new line number for context lines, -1 for deletions */
	int adjacent_new; /* for deletions: new line at the deletion position */
	size_t hunk_idx;  /* hunk that contains this line */
} linemap_old_entry;

typedef struct {
	int new_lineno;   /* 1-based new line number */
	int old_lineno;   /* old line number for context lines, -1 for additions */
	int adjacent_old; /* for additions: old line the addition was inserted before */
	size_t hunk_idx;  /* hunk that contains this line */
} linemap_new_entry;

struct git_diff_linemap {
	git_delta_t delta_status;
	char *old_path;
	char *new_path;
	bool binary;

	git_array_t(linemap_hunk) hunks;
	git_array_t(linemap_old_entry) old_entries;
	git_array_t(linemap_new_entry) new_entries;
};

static int linemap_copy_hunks(git_diff_linemap *map, git_patch *patch)
{
	size_t hunk_count, h;
	int error = 0;

	hunk_count = git_patch_num_hunks(patch);

	for (h = 0; h < hunk_count; h++) {
		const git_diff_hunk *hunk;
		size_t line_count, l;
		linemap_hunk *copy;
		int cur_old, cur_new;

		if ((error = git_patch_get_hunk(&hunk, &line_count, patch, h)) < 0)
			return error;

		copy = git_array_alloc(map->hunks);
		GIT_ERROR_CHECK_ALLOC(copy);

		copy->old_start = hunk->old_start;
		copy->old_lines = hunk->old_lines;
		copy->new_start = hunk->new_start;
		copy->new_lines = hunk->new_lines;

		/*
		 * When a hunk has no lines on one side (a pure addition or
		 * pure deletion hunk), the start line on that side is the
		 * line *after which* the hunk sits; adjust the counters so
		 * that adjacent positions are always reported as the 1-based
		 * line before which an addition sits, or the line that now
		 * sits where a deletion happened.
		 */
		cur_old = hunk->old_start + (hunk->old_lines == 0 ? 1 : 0);
		cur_new = hunk->new_start + (hunk->new_lines == 0 ? 1 : 0);

		for (l = 0; l < line_count; l++) {
			const git_diff_line *line;

			if ((error = git_patch_get_line_in_hunk(&line, patch, h, l)) < 0)
				return error;

			switch (line->origin) {
			case GIT_DIFF_LINE_CONTEXT: {
				linemap_old_entry *old_entry =
					git_array_alloc(map->old_entries);
				linemap_new_entry *new_entry =
					git_array_alloc(map->new_entries);

				GIT_ERROR_CHECK_ALLOC(old_entry);
				GIT_ERROR_CHECK_ALLOC(new_entry);

				old_entry->old_lineno = line->old_lineno;
				old_entry->new_lineno = line->new_lineno;
				old_entry->adjacent_new = -1;
				old_entry->hunk_idx = h;

				new_entry->new_lineno = line->new_lineno;
				new_entry->old_lineno = line->old_lineno;
				new_entry->adjacent_old = -1;
				new_entry->hunk_idx = h;

				cur_old++;
				cur_new++;
				break;
			}
			case GIT_DIFF_LINE_DELETION: {
				linemap_old_entry *old_entry =
					git_array_alloc(map->old_entries);

				GIT_ERROR_CHECK_ALLOC(old_entry);

				old_entry->old_lineno = line->old_lineno;
				old_entry->new_lineno = -1;
				old_entry->adjacent_new = cur_new;
				old_entry->hunk_idx = h;

				cur_old++;
				break;
			}
			case GIT_DIFF_LINE_ADDITION: {
				linemap_new_entry *new_entry =
					git_array_alloc(map->new_entries);

				GIT_ERROR_CHECK_ALLOC(new_entry);

				new_entry->new_lineno = line->new_lineno;
				new_entry->old_lineno = -1;
				new_entry->adjacent_old = cur_old;
				new_entry->hunk_idx = h;

				cur_new++;
				break;
			}
			default:
				/*
				 * GIT_DIFF_LINE_{CONTEXT,ADD,DEL}_EOFNL are
				 * pseudo lines (the no-newline-at-EOF marker),
				 * not actual file lines; they carry no mapping
				 * information.
				 */
				break;
			}
		}
	}

	return 0;
}

int git_diff_linemap_new(
	git_diff_linemap **out,
	git_diff *diff,
	size_t delta_idx)
{
	git_diff_linemap *map = NULL;
	const git_diff_delta *delta;
	git_patch *patch = NULL;
	int error = 0;

	if (!out || !diff) {
		git_error_set(GIT_ERROR_INVALID, "linemap out or diff is NULL");
		return -1;
	}

	*out = NULL;

	if (delta_idx >= git_diff_num_deltas(diff)) {
		git_error_set(GIT_ERROR_INVALID,
			"delta index %" PRIuZ " out of range", delta_idx);
		return GIT_ENOTFOUND;
	}

	delta = git_diff_get_delta(diff, delta_idx);
	GIT_ERROR_CHECK_ALLOC(delta);

	map = git__calloc(1, sizeof(git_diff_linemap));
	GIT_ERROR_CHECK_ALLOC(map);

	map->delta_status = delta->status;

	if (delta->old_file.path) {
		map->old_path = git__strdup(delta->old_file.path);
		if (!map->old_path) {
			error = -1;
			goto done;
		}
	}

	if (delta->new_file.path) {
		map->new_path = git__strdup(delta->new_file.path);
		if (!map->new_path) {
			error = -1;
			goto done;
		}
	}

	/*
	 * Generating the patch loads the file contents, which is also
	 * what updates the binary flag on the delta; check the flag
	 * only afterwards.  Binary deltas carry no usable hunk data;
	 * record the flag so that queries can report
	 * GIT_DIFF_LINEMAP_UNMAPPABLE.
	 */
	if ((error = git_patch_from_diff(&patch, diff, delta_idx)) < 0)
		goto done;

	map->binary = (delta->flags & GIT_DIFF_FLAG_BINARY) != 0;

	if (!map->binary &&
	    (error = linemap_copy_hunks(map, patch)) < 0)
		goto done;

	*out = map;
	map = NULL;

done:
	git_patch_free(patch);
	git_diff_linemap_free(map);
	return error;
}

/*
 * The number of old (or new) file lines up to and including a hunk.
 *
 * For a hunk with lines on that side, this is the last covered line
 * (start + lines - 1); for a hunk with no lines on that side (a pure
 * addition or deletion), the start line is the line after which the
 * hunk sits, which is exactly the count of lines before the hunk.
 */
static int linemap_hunk_old_pos_after(const linemap_hunk *h)
{
	return h->old_start + (h->old_lines > 0 ? h->old_lines - 1 : 0);
}

static int linemap_hunk_new_pos_after(const linemap_hunk *h)
{
	return h->new_start + (h->new_lines > 0 ? h->new_lines - 1 : 0);
}

/*
 * Find the last hunk whose old range ends before `old_lineno`; the
 * queried line (which is not inside any hunk) is shifted by the
 * cumulative offset of that hunk.
 */
static int linemap_offset_for_old_line(const git_diff_linemap *map, int old_lineno)
{
	int offset = 0;
	size_t i;

	for (i = 0; i < git_array_size(map->hunks); i++) {
		const linemap_hunk *h = &map->hunks.ptr[i];
		int old_end = linemap_hunk_old_pos_after(h);

		if (old_lineno > old_end)
			offset = linemap_hunk_new_pos_after(h) - old_end;
		else
			break;
	}

	return offset;
}

static int linemap_offset_for_new_line(const git_diff_linemap *map, int new_lineno)
{
	int offset = 0;
	size_t i;

	for (i = 0; i < git_array_size(map->hunks); i++) {
		const linemap_hunk *h = &map->hunks.ptr[i];
		int new_end = linemap_hunk_new_pos_after(h);

		if (new_lineno > new_end)
			offset = linemap_hunk_old_pos_after(h) - new_end;
		else
			break;
	}

	return offset;
}

static const linemap_old_entry *linemap_find_old_entry(
	const git_diff_linemap *map, int old_lineno)
{
	size_t lo = 0, hi = git_array_size(map->old_entries);

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		const linemap_old_entry *entry = &map->old_entries.ptr[mid];

		if (entry->old_lineno == old_lineno)
			return entry;
		else if (entry->old_lineno < old_lineno)
			lo = mid + 1;
		else
			hi = mid;
	}

	return NULL;
}

static const linemap_new_entry *linemap_find_new_entry(
	const git_diff_linemap *map, int new_lineno)
{
	size_t lo = 0, hi = git_array_size(map->new_entries);

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		const linemap_new_entry *entry = &map->new_entries.ptr[mid];

		if (entry->new_lineno == new_lineno)
			return entry;
		else if (entry->new_lineno < new_lineno)
			lo = mid + 1;
		else
			hi = mid;
	}

	return NULL;
}

static void linemap_result_reset(git_diff_linemap_result *out)
{
	memset(out, 0, sizeof(*out));
	out->status = GIT_DIFF_LINEMAP_UNMAPPABLE;
	out->lineno = -1;
	out->hunk_idx = (size_t)-1;
	out->adjacent_lineno = -1;
}

int git_diff_linemap_old_to_new(
	git_diff_linemap_result *out,
	const git_diff_linemap *map,
	int old_lineno)
{
	const linemap_old_entry *entry;

	if (!out)
		return -1;

	linemap_result_reset(out);

	if (!map || old_lineno < 1) {
		git_error_set(GIT_ERROR_INVALID,
			"invalid linemap or line number");
		return -1;
	}

	if (map->binary)
		return 0;

	if ((entry = linemap_find_old_entry(map, old_lineno)) != NULL) {
		if (entry->new_lineno > 0) {
			out->status = GIT_DIFF_LINEMAP_OK;
			out->lineno = entry->new_lineno;
		} else {
			out->status = GIT_DIFF_LINEMAP_DELETED;
			out->hunk_idx = entry->hunk_idx;
			out->adjacent_lineno = entry->adjacent_new;
		}

		return 0;
	}

	/* outside of all hunks: unchanged line, shifted by the hunks above */
	out->status = GIT_DIFF_LINEMAP_OK;
	out->lineno = old_lineno + linemap_offset_for_old_line(map, old_lineno);

	return 0;
}

int git_diff_linemap_new_to_old(
	git_diff_linemap_result *out,
	const git_diff_linemap *map,
	int new_lineno)
{
	const linemap_new_entry *entry;

	if (!out)
		return -1;

	linemap_result_reset(out);

	if (!map || new_lineno < 1) {
		git_error_set(GIT_ERROR_INVALID,
			"invalid linemap or line number");
		return -1;
	}

	if (map->binary)
		return 0;

	if ((entry = linemap_find_new_entry(map, new_lineno)) != NULL) {
		if (entry->old_lineno > 0) {
			out->status = GIT_DIFF_LINEMAP_OK;
			out->lineno = entry->old_lineno;
		} else {
			out->status = GIT_DIFF_LINEMAP_ADDED;
			out->hunk_idx = entry->hunk_idx;
			out->adjacent_lineno = entry->adjacent_old;
		}

		return 0;
	}

	/* outside of all hunks: unchanged line, shifted by the hunks above */
	out->status = GIT_DIFF_LINEMAP_OK;
	out->lineno = new_lineno + linemap_offset_for_new_line(map, new_lineno);

	return 0;
}

void git_diff_linemap_free(git_diff_linemap *map)
{
	if (!map)
		return;

	git__free(map->old_path);
	git__free(map->new_path);
	git_array_clear(map->hunks);
	git_array_clear(map->old_entries);
	git_array_clear(map->new_entries);
	git__free(map);
}
