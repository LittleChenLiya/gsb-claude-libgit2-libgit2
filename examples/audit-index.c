/*
 * libgit2 "audit-index" example - read-only index audit
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

/*
 * A read-only audit of the repository index.
 *
 * Every index entry is classified by its real stage and flags:
 *   - normal:            stage 0 with staged content and the worktree
 *                        file present
 *   - intent-to-add:     stage 0 carrying GIT_INDEX_ENTRY_INTENT_TO_ADD
 *   - conflict:          stage 1/2/3 (ancestor / ours / theirs)
 *   - missing-worktree:  stage 0 whose worktree file is absent
 *
 * Case-folded path collisions (e.g. "Foo.txt" vs "foo.txt") are reported
 * as an independent diagnostic; they never change an entry's class and
 * never merge or drop records.
 *
 * The audit only reads.  It opens the index once and never calls
 * git_index_read(force), git_index_write, git_index_add* or
 * git_index_remove*, and never creates, modifies or deletes a worktree
 * file.  A missing worktree file is reported as such; it is never
 * treated as a deletion of staged content.
 */

typedef enum {
	AUDIT_NORMAL = 0,
	AUDIT_INTENT_TO_ADD,
	AUDIT_CONFLICT,
	AUDIT_MISSING_WORKTREE
} audit_class;

typedef struct {
	char *path;
	unsigned int stage;
	uint16_t flags;
	uint16_t flags_extended;
	char oid[GIT_OID_SHA1_HEXSIZE + 1];
	int intent_to_add;
	int skip_worktree;
	int worktree_present; /* 1 yes, 0 no, -1 n/a (bare repository) */
	audit_class klass;
} audit_record;

typedef struct {
	char *path_a;
	char *path_b;
} audit_collision;

typedef struct {
	size_t normal;
	size_t intent_to_add;
	size_t conflict;
	size_t missing_worktree;
	size_t conflict_paths;
	size_t collisions;
} audit_summary;

static const char *class_name(audit_class klass)
{
	switch (klass) {
	case AUDIT_NORMAL:
		return "normal";
	case AUDIT_INTENT_TO_ADD:
		return "intent-to-add";
	case AUDIT_CONFLICT:
		return "conflict";
	case AUDIT_MISSING_WORKTREE:
		return "missing-worktree";
	}
	return "unknown";
}

/*
 * Presence of the worktree file for a stage-0 entry.  This only stats
 * the file; it never reads or hashes it and never touches the index.
 * Returns 1 present, 0 absent, -1 not applicable (bare repo / no workdir).
 */
static int worktree_file_present(git_repository *repo, const char *path)
{
	const char *workdir;
	char full[GIT_PATH_MAX];
	struct stat st;

	if (git_repository_is_bare(repo))
		return -1;

	workdir = git_repository_workdir(repo);
	if (!workdir)
		return -1;

	if (snprintf(full, sizeof(full), "%s%s", workdir, path) >= (int)sizeof(full))
		return 0;

	return (stat(full, &st) == 0 && S_ISREG(st.st_mode));
}

static audit_class classify_entry(git_repository *repo,
	const git_index_entry *entry, int *worktree_present)
{
	unsigned int stage = git_index_entry_stage(entry);
	int intent_to_add =
		(entry->flags_extended & GIT_INDEX_ENTRY_INTENT_TO_ADD) != 0;

	if (stage > 0) {
		*worktree_present = -1;
		return AUDIT_CONFLICT;
	}

	if (intent_to_add) {
		*worktree_present = worktree_file_present(repo, entry->path);
		return AUDIT_INTENT_TO_ADD;
	}

	*worktree_present = worktree_file_present(repo, entry->path);
	if (*worktree_present == 0)
		return AUDIT_MISSING_WORKTREE;

	return AUDIT_NORMAL;
}

static int record_collision(audit_collision **out, size_t *count,
	const char *path_a, const char *path_b)
{
	audit_collision *grown;

	grown = realloc(*out, (*count + 1) * sizeof(audit_collision));
	if (!grown)
		return -1;
	*out = grown;

	(*out)[*count].path_a = strdup(path_a);
	(*out)[*count].path_b = strdup(path_b);
	if (!(*out)[*count].path_a || !(*out)[*count].path_b)
		return -1;

	(*count)++;
	return 0;
}

/*
 * Detect case-folded path collisions as an independent diagnostic.
 * Records keep their own stage, OID and flags; collisions never affect
 * classification.
 */
static int detect_collisions(audit_record *records, size_t count,
	audit_collision **collisions, size_t *collision_count)
{
	size_t i, j;

	for (i = 0; i < count; i++) {
		for (j = i + 1; j < count; j++) {
			if (strcmp(records[i].path, records[j].path) == 0)
				continue; /* same path: stages of one conflict */
#ifdef _WIN32
			if (_stricmp(records[i].path, records[j].path) == 0)
#else
			if (strcasecmp(records[i].path, records[j].path) == 0)
#endif
			{
				if (record_collision(collisions, collision_count,
					    records[i].path, records[j].path) < 0)
					return -1;
			}
		}
	}
	return 0;
}

static void free_collisions(audit_collision *collisions, size_t count)
{
	size_t i;
	for (i = 0; i < count; i++) {
		free(collisions[i].path_a);
		free(collisions[i].path_b);
	}
	free(collisions);
}

static void print_summary(const audit_summary *summary, int json)
{
	int committable = (summary->conflict_paths == 0);

	if (json) {
		printf("\"summary\":{\"normal\":%zu,\"intent_to_add\":%zu,"
			"\"conflict_entries\":%zu,\"conflict_paths\":%zu,"
			"\"missing_worktree\":%zu,\"case_collisions\":%zu,"
			"\"committable\":%s}\n",
			summary->normal, summary->intent_to_add,
			summary->conflict, summary->conflict_paths,
			summary->missing_worktree, summary->collisions,
			committable ? "true" : "false");
		return;
	}

	printf("Summary:\n");
	printf("  normal entries:        %zu\n", summary->normal);
	printf("  intent-to-add:         %zu (no staged content)\n",
		summary->intent_to_add);
	printf("  conflict entries:      %zu (%zu unmerged paths)\n",
		summary->conflict, summary->conflict_paths);
	printf("  missing worktree:      %zu (staged OID intact, not a deletion)\n",
		summary->missing_worktree);
	printf("  case collisions:       %zu (diagnostic only)\n",
		summary->collisions);
	printf("  committable:           %s\n",
		committable ? "yes" : "no (unmerged entries present)");
}

int lg2_audit_index(git_repository *repo, int argc, char **argv)
{
	git_index *index = NULL;
	audit_record *records = NULL;
	audit_collision *collisions = NULL;
	audit_summary summary;
	size_t i, j, count, collision_count = 0;
	int json = 0;
	int error = 0;

	if (argc > 1) {
		if (!strcmp(argv[1], "--json"))
			json = 1;
		else
			fatal("usage: audit-index [--json]", NULL);
	}

	memset(&summary, 0, sizeof(summary));

	check_lg2(git_repository_index(&index, repo),
		"could not open repository index", NULL);

	count = git_index_entrycount(index);
	records = calloc(count ? count : 1, sizeof(audit_record));
	if (!records) {
		error = -1;
		goto out;
	}

	for (i = 0; i < count; i++) {
		const git_index_entry *entry = git_index_get_byindex(index, i);
		audit_record *record = &records[i];

		record->path = strdup(entry->path);
		if (!record->path) {
			error = -1;
			goto out;
		}

		record->stage = git_index_entry_stage(entry);
		record->flags = entry->flags;
		record->flags_extended = entry->flags_extended;
		git_oid_fmt(record->oid, &entry->id);
		record->oid[GIT_OID_SHA1_HEXSIZE] = '\0';
		record->intent_to_add =
			(entry->flags_extended & GIT_INDEX_ENTRY_INTENT_TO_ADD) != 0;
		record->skip_worktree =
			(entry->flags_extended & GIT_INDEX_ENTRY_SKIP_WORKTREE) != 0;
		record->klass = classify_entry(repo, entry, &record->worktree_present);

		switch (record->klass) {
		case AUDIT_NORMAL:
			summary.normal++;
			break;
		case AUDIT_INTENT_TO_ADD:
			summary.intent_to_add++;
			break;
		case AUDIT_CONFLICT:
			summary.conflict++;
			break;
		case AUDIT_MISSING_WORKTREE:
			summary.missing_worktree++;
			break;
		}
	}

	/* count distinct unmerged paths */
	for (i = 0; i < count; i++) {
		if (records[i].klass != AUDIT_CONFLICT)
			continue;
		for (j = 0; j < i; j++) {
			if (records[j].klass == AUDIT_CONFLICT &&
			    !strcmp(records[j].path, records[i].path))
				break;
		}
		if (j == i)
			summary.conflict_paths++;
	}

	if (detect_collisions(records, count, &collisions, &collision_count) < 0) {
		error = -1;
		goto out;
	}
	summary.collisions = collision_count;

	if (json) {
		printf("{\n  \"records\": [\n");
		for (i = 0; i < count; i++) {
			audit_record *record = &records[i];
			printf("    {\"path\":\"%s\",\"stage\":%u,"
				"\"oid\":\"%s\",\"flags\":%u,"
				"\"flags_extended\":%u,"
				"\"intent_to_add\":%s,\"skip_worktree\":%s,"
				"\"worktree_present\":%s,\"class\":\"%s\"}%s\n",
				record->path, record->stage, record->oid,
				record->flags, record->flags_extended,
				record->intent_to_add ? "true" : "false",
				record->skip_worktree ? "true" : "false",
				record->worktree_present < 0 ? "null" :
					(record->worktree_present ? "true" : "false"),
				class_name(record->klass),
				(i + 1 < count) ? "," : "");
		}
		printf("  ],\n");

		printf("  \"collisions\": [");
		for (i = 0; i < collision_count; i++) {
			printf("%s{\"path_a\":\"%s\",\"path_b\":\"%s\"}",
				i ? "," : " ",
				collisions[i].path_a, collisions[i].path_b);
		}
		printf(" ],\n");
	} else {
		printf("Read-only index audit for %s\n", git_index_path(index));
		printf("entries: %zu\n\n", count);

		for (i = 0; i < count; i++) {
			audit_record *record = &records[i];
			printf("  %s\n", record->path);
			printf("    stage: %u  oid: %s\n",
				record->stage, record->oid);
			printf("    flags: 0x%04x  flags_extended: 0x%04x\n",
				record->flags, record->flags_extended);
			printf("    class: %s  worktree: %s%s%s\n",
				class_name(record->klass),
				record->worktree_present < 0 ? "n/a" :
					(record->worktree_present ? "present" : "MISSING"),
				record->intent_to_add ? "  intent-to-add" : "",
				record->skip_worktree ? "  skip-worktree" : "");
		}
		printf("\n");

		if (collision_count > 0) {
			printf("Case collisions (diagnostic, stage identity unchanged):\n");
			for (i = 0; i < collision_count; i++) {
				printf("  '%s' <-> '%s'\n",
					collisions[i].path_a, collisions[i].path_b);
			}
			printf("\n");
		}
	}

	print_summary(&summary, json);
	if (json)
		printf("}\n");

out:
	if (records) {
		for (i = 0; i < count; i++)
			free(records[i].path);
		free(records);
	}
	free_collisions(collisions, collision_count);
	git_index_free(index);
	return error;
}
