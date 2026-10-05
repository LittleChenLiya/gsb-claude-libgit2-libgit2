/*
 * libgit2 "odb-read-session" example - shows how to read objects through
 * an explicit, budgeted read session.
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
 * This example demonstrates a read session: an explicit, budgeted context
 * for reading objects from the object database.  The session enforces the
 * allowed object types, a per-object byte cap, and a total byte budget for
 * the session.  It traverses references along caller-selected edges and
 * can deduplicate objects referenced more than once while still reporting
 * every source.
 *
 * The example creates a tree with three blobs and traverses it through a
 * session that follows tree->blob edges and has a budget large enough for
 * all three objects.  It then prints each delivered object and the running
 * byte total.
 *
 * Recognized options are:
 *   --budget <bytes>: total byte budget for the session (default: 100)
 *   --no-dedup: do not deduplicate objects referenced more than once
 */

struct session_opts {
	git_object_size_t budget;
	int dedup;
};

static void parse_opts(struct session_opts *opts, int argc, char **argv)
{
	struct args_info args = ARGS_INFO_INIT;

	opts->budget = 100;
	opts->dedup = 1;

	for (args.pos = 1; args.pos < argc; ++args.pos) {
		const char *a = argv[args.pos];

		if (!strcmp(a, "--budget")) {
			if (args.pos + 1 >= argc)
				fatal("missing argument for --budget", NULL);
			opts->budget = strtoull(argv[++args.pos], NULL, 10);
		} else if (!strcmp(a, "--no-dedup")) {
			opts->dedup = 0;
		}
	}
}

struct traverse_report {
	int delivered;
	int duplicates;
	git_object_size_t bytes;
};

static int report_cb(git_odb_object *obj, const git_oid *source,
	unsigned int flags, void *payload)
{
	struct traverse_report *report = (struct traverse_report *)payload;
	char oidstr[GIT_OID_SHA1_HEXSIZE + 1];

	(void)source;

	git_oid_tostr(oidstr, sizeof(oidstr), git_odb_object_id(obj));
	if (flags & GIT_ODB_READ_SESSION_OBJ_DELIVERED) {
		report->delivered++;
		report->bytes += git_odb_object_size(obj);
		printf("  delivered %s %s (%zu bytes) [total %" PRIu64 "]\n",
			git_object_type2string(git_odb_object_type(obj)),
			oidstr, git_odb_object_size(obj), report->bytes);
	}
	if (flags & GIT_ODB_READ_SESSION_OBJ_DUPLICATE) {
		report->duplicates++;
		printf("  duplicate reference to %s (not billed)\n", oidstr);
	}
	return 0;
}

int lg2_odb_read_session(git_repository *repo, int argc, char **argv)
{
	struct session_opts opts;
	git_odb *odb = NULL;
	git_odb_read_session *session = NULL;
	git_odb_read_session_options session_opts = GIT_ODB_READ_SESSION_OPTIONS_INIT;
	git_oid blob1, blob2, blob3, tree_id;
	git_treebuilder *builder = NULL;
	struct traverse_report report;
	int error = 0;

	parse_opts(&opts, argc, argv);

	memset(&report, 0, sizeof(report));

	check_lg2(git_repository_odb(&odb, repo), "could not get object database", NULL);

	/* Create three blobs. */
	check_lg2(git_odb_write(&blob1, odb, "one", 3, GIT_OBJECT_BLOB),
		"could not write blob", NULL);
	check_lg2(git_odb_write(&blob2, odb, "two", 3, GIT_OBJECT_BLOB),
		"could not write blob", NULL);
	check_lg2(git_odb_write(&blob3, odb, "three", 5, GIT_OBJECT_BLOB),
		"could not write blob", NULL);

	/* Build a tree that points at all three blobs. */
	check_lg2(git_treebuilder_new(&builder, repo, NULL),
		"could not create tree builder", NULL);
	check_lg2(git_treebuilder_insert(NULL, builder, "a.txt", &blob1, GIT_FILEMODE_BLOB),
		"could not insert tree entry", NULL);
	check_lg2(git_treebuilder_insert(NULL, builder, "b.txt", &blob2, GIT_FILEMODE_BLOB),
		"could not insert tree entry", NULL);
	check_lg2(git_treebuilder_insert(NULL, builder, "c.txt", &blob3, GIT_FILEMODE_BLOB),
		"could not insert tree entry", NULL);
	check_lg2(git_treebuilder_write(&tree_id, builder),
		"could not write tree", NULL);
	git_treebuilder_free(builder);
	builder = NULL;

	/* Open a read session and traverse the tree. */
	session_opts.allowed_types = (1u << GIT_OBJECT_BLOB) | (1u << GIT_OBJECT_TREE);
	session_opts.traverse = GIT_ODB_TRAVERSE_TREE_BLOBS;
	session_opts.max_total_size = opts.budget;
	if (opts.dedup)
		session_opts.flags |= GIT_ODB_READ_SESSION_DEDUP_REFERENCES;

	check_lg2(git_odb_read_session_new(&session, odb, &session_opts),
		"could not create read session", NULL);

	printf("Traversing tree with budget %" PRIu64 " bytes:\n", opts.budget);
	error = git_odb_read_session_traverse(session, &tree_id, report_cb, &report);
	if (error == GIT_EINVALID) {
		printf("  traversal stopped: budget or limit exceeded\n");
	} else if (error) {
		check_lg2(error, "traversal failed", NULL);
	}

	printf("Session summary:\n");
	printf("  objects delivered: %zu\n", git_odb_read_session_object_count(session));
	printf("  bytes delivered:   %" PRIu64 "\n", git_odb_read_session_delivered(session));
	printf("  duplicate sources:  %d\n", report.duplicates);

	git_odb_read_session_free(session);
	git_odb_free(odb);

	return 0;
}
