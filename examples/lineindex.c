/*
 * libgit2 "lineindex" example - shows how to map line numbers between
 * two versions of a file using the line index API
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
 * This example demonstrates the `git_lineindex` API: given a diff between
 * two trees, it builds a line index and prints where each old file line
 * ended up in the new file.  Deleted lines are reported as deleted (with
 * their position in the new file) and added lines can be reverse-queried.
 *
 * Usage: lg2 lineindex <old-treeish> <new-treeish> [path]
 */

static void print_usage(void)
{
	fprintf(stderr, "usage: lg2 lineindex <old-treeish> <new-treeish> [path]\n");
}

static int parse_treeish(git_tree **out, git_repository *repo, const char *spec)
{
	git_object *obj = NULL;
	int error;

	if ((error = git_revparse_single(&obj, repo, spec)) < 0)
		return error;

	error = git_object_peel((git_object **)out, obj, GIT_OBJECT_TREE);
	git_object_free(obj);
	return error;
}

int lg2_lineindex(git_repository *repo, int argc, char **argv)
{
	git_tree *old_tree = NULL, *new_tree = NULL;
	git_diff *diff = NULL;
	git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
	git_lineindex *idx = NULL;
	const char *path = NULL;
	size_t d, ndeltas;
	int error = 0;

	if (argc < 3) {
		print_usage();
		return 1;
	}

	if (argc > 3)
		path = argv[3];

	if ((error = parse_treeish(&old_tree, repo, argv[1])) < 0 ||
	    (error = parse_treeish(&new_tree, repo, argv[2])) < 0)
		goto out;

	if (path) {
		opts.flags |= GIT_DIFF_DISABLE_PATHSPEC_MATCH;
		opts.pathspec.strings = (char **)&path;
		opts.pathspec.count = 1;
	}

	if ((error = git_diff_tree_to_tree(&diff, repo,
			old_tree, new_tree, &opts)) < 0)
		goto out;

	if ((error = git_lineindex_new(&idx, diff)) < 0)
		goto out;

	/* The index copies everything it needs; the diff may be freed. */
	git_diff_free(diff);
	diff = NULL;

	ndeltas = git_lineindex_delta_count(idx);
	for (d = 0; d < ndeltas; d++) {
		git_lineindex_delta_info info;
		int ln;

		if ((error = git_lineindex_get_delta_info(idx, d, &info)) < 0)
			goto out;

		printf("%s -> %s (%s, %d old lines, %d new lines)\n",
			info.old_path ? info.old_path : "(none)",
			info.new_path ? info.new_path : "(none)",
			info.binary ? "binary" : "text",
			info.old_lines, info.new_lines);

		if (info.binary) {
			printf("  binary file, no line mapping\n");
			continue;
		}

		for (ln = 1; ln <= info.old_lines; ln++) {
			git_lineindex_location loc;

			if (git_lineindex_lookup_old(idx, d, ln, &loc) < 0)
				goto out;

			switch (loc.status) {
			case GIT_LINEINDEX_CONTEXT:
				printf("  old line %d -> new line %d\n",
					ln, loc.new_lineno);
				break;
			case GIT_LINEINDEX_DELETED:
				printf("  old line %d -> DELETED "
					"(between new line %d and %d)\n",
					ln, loc.before, loc.after);
				break;
			default:
				printf("  old line %d -> unmappable\n", ln);
				break;
			}
		}
	}

out:
	git_lineindex_free(idx);
	git_diff_free(diff);
	git_tree_free(new_tree);
	git_tree_free(old_tree);
	return error;
}
