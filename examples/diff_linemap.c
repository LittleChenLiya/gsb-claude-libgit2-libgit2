/*
 * libgit2 "diff-linemap" example - shows how to use the diff line
 * mapping index API
 *
 * Written by the libgit2 contributors
 *
 * To the extent possible under law, the author(s) have dedicated all copyright
 * and related and neighboring rights to this software to the public domain
 * worldwide. This software is distributed without any warranty.
 *
 * You should have received a copy of the CC0 Public Domain Dedication along
 * with this software. If not, see
 * <http://creativecommons.org/publicdomain/zero/1.0/>.
 */

#include "common.h"

/**
 * This example demonstrates the `git_diff_linemap` API, which builds
 * an index on top of a single delta of a diff that translates line
 * numbers between the old and the new version of a file (for example,
 * to migrate offline code review comments across a diff).
 *
 * It diffs the index against the workdir and, for every modified
 * file, prints how the first few old-side lines map to the new
 * version and how the first few new-side lines map back.
 *
 * Usage: lg2 diff-linemap
 */

#define DEMO_LINES 8

static void print_old_to_new(const git_diff_linemap *map, int old_lineno)
{
	git_diff_linemap_result result;

	if (git_diff_linemap_old_to_new(&result, map, old_lineno) < 0)
		return;

	switch (result.status) {
	case GIT_DIFF_LINEMAP_OK:
		printf("  old line %d -> new line %d\n", old_lineno, result.lineno);
		break;
	case GIT_DIFF_LINEMAP_DELETED:
		printf("  old line %d -> deleted (hunk %d, new position %d)\n",
			old_lineno, (int)result.hunk_idx, result.adjacent_lineno);
		break;
	default:
		printf("  old line %d -> unmappable\n", old_lineno);
		break;
	}
}

static void print_new_to_old(const git_diff_linemap *map, int new_lineno)
{
	git_diff_linemap_result result;

	if (git_diff_linemap_new_to_old(&result, map, new_lineno) < 0)
		return;

	switch (result.status) {
	case GIT_DIFF_LINEMAP_OK:
		printf("  new line %d -> old line %d\n", new_lineno, result.lineno);
		break;
	case GIT_DIFF_LINEMAP_ADDED:
		printf("  new line %d -> added (hunk %d, before old line %d)\n",
			new_lineno, (int)result.hunk_idx, result.adjacent_lineno);
		break;
	default:
		printf("  new line %d -> unmappable\n", new_lineno);
		break;
	}
}

int lg2_diff_linemap(git_repository *repo, int argc, char **argv)
{
	git_diff *diff = NULL;
	size_t i, deltas;
	int error = 0;

	UNUSED(argc);
	UNUSED(argv);

	/* diff the index against the workdir */
	if ((error = git_diff_index_to_workdir(&diff, repo, NULL, NULL)) < 0)
		goto out;

	deltas = git_diff_num_deltas(diff);

	for (i = 0; i < deltas; i++) {
		const git_diff_delta *delta = git_diff_get_delta(diff, i);
		git_diff_linemap *map = NULL;
		int line;

		if (delta->status != GIT_DELTA_MODIFIED)
			continue;

		printf("file: %s\n", delta->new_file.path);

		if ((error = git_diff_linemap_new(&map, diff, (size_t)i)) < 0)
			goto out;

		for (line = 1; line <= DEMO_LINES; line++)
			print_old_to_new(map, line);
		for (line = 1; line <= DEMO_LINES; line++)
			print_new_to_old(map, line);

		/*
		 * the index holds its own copies of the hunk data, so it
		 * would stay usable even after git_diff_free(diff); here
		 * we free it right away since we are done with it.
		 */
		git_diff_linemap_free(map);
	}

out:
	git_diff_free(diff);
	return error;
}
