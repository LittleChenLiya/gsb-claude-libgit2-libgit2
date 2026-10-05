/*
 * Read-only index audit.
 *
 * The audit classifies every index entry into exactly one of:
 *
 *   - AUDIT_ENTRY_NORMAL:           stage 0 entry whose worktree file exists
 *   - AUDIT_ENTRY_INTENT_TO_ADD:    stage 0 entry with the intent-to-add flag
 *   - AUDIT_ENTRY_CONFLICT:         entry at conflict stage 1/2/3
 *   - AUDIT_ENTRY_MISSING_WORKTREE: stage 0 entry whose worktree file is gone
 *                                   (the staged content is NOT removed; the
 *                                   entry keeps its OID and flags)
 *
 * Invariants:
 *
 *   - the audit is strictly read-only: it never refreshes, writes or
 *     otherwise mutates the index, and never touches the worktree;
 *     missing worktree content is detected with a plain stat, never by
 *     refreshing the index;
 *   - every record keeps the entry's full OID and its raw flags /
 *     extended flags;
 *   - case-insensitive path collisions are reported as an independent
 *     diagnostic only; they never override an entry's real stage
 *     identity (which comes solely from the stage bits in the flags);
 *   - the summary reports whether the index is committable (ie, free of
 *     conflict entries).
 */

#include "clar_libgit2.h"
#include "futils.h"
#include "fs_path.h"

typedef enum {
	AUDIT_ENTRY_NORMAL = 0,
	AUDIT_ENTRY_INTENT_TO_ADD,
	AUDIT_ENTRY_CONFLICT,
	AUDIT_ENTRY_MISSING_WORKTREE
} audit_entry_state;

typedef struct {
	const char *path;        /* borrowed from the index entry */
	git_oid oid;             /* full object id of the staged content */
	uint16_t flags;          /* raw on-disk flags (incl. stage bits) */
	uint16_t flags_extended; /* raw extended flags (intent-to-add, ...) */
	int stage;               /* decoded stage (0..3) */
	audit_entry_state state;
} audit_record;

typedef struct {
	audit_record *records;
	size_t record_count;

	size_t normal_count;
	size_t intent_to_add_count;
	size_t conflict_entry_count;  /* stage > 0 entries */
	size_t conflicted_path_count; /* distinct paths with stage > 0 entries */
	size_t missing_worktree_count;

	/* independent diagnostic only; never affects classification */
	size_t case_collision_count;
	git_str case_collisions;

	bool committable; /* no conflict entries in the index */
} audit_report;

static void audit_report_dispose(audit_report *report)
{
	if (!report)
		return;

	git__free(report->records);
	git_str_dispose(&report->case_collisions);
	memset(report, 0, sizeof(*report));
}


/*
 * Audit the given index without modifying it, the index file on disk,
 * or the worktree.  Only git_index_entrycount / git_index_get_byindex
 * and a read-only stat of worktree paths are used; in particular the
 * index is never refreshed to "see" missing content.
 */
static int audit_index(git_repository *repo, git_index *index, audit_report *out)
{
	const char *workdir = git_repository_workdir(repo);
	git_str fullpath = GIT_STR_INIT;
	size_t i, j, count;
	int error = 0;

	GIT_ASSERT_ARG(repo);
	GIT_ASSERT_ARG(index);
	GIT_ASSERT_ARG(out);

	memset(out, 0, sizeof(*out));

	count = git_index_entrycount(index);
	if (count > 0) {
		out->records = git__calloc(count, sizeof(audit_record));
		GIT_ERROR_CHECK_ALLOC(out->records);
	}
	out->record_count = count;

	for (i = 0; i < count; i++) {
		const git_index_entry *entry = git_index_get_byindex(index, i);
		audit_record *rec = &out->records[i];
		int stage = git_index_entry_stage(entry);

		rec->path = entry->path;
		git_oid_cpy(&rec->oid, &entry->id);
		rec->flags = entry->flags;
		rec->flags_extended = entry->flags_extended;
		rec->stage = stage;

		if (stage != 0) {
			/* conflict identity comes solely from the stage bits */
			rec->state = AUDIT_ENTRY_CONFLICT;
			out->conflict_entry_count++;
			if (i == 0 || strcmp(out->records[i - 1].path, entry->path) != 0)
				out->conflicted_path_count++;
		} else if ((entry->flags_extended & GIT_INDEX_ENTRY_INTENT_TO_ADD) != 0) {
			rec->state = AUDIT_ENTRY_INTENT_TO_ADD;
			out->intent_to_add_count++;
		} else {
			/* read-only existence check; never refresh the index */
			git_str_clear(&fullpath);
			if ((error = git_str_joinpath(&fullpath, workdir, entry->path)) < 0)
				goto done;

			if (!git_fs_path_exists(fullpath.ptr)) {
				rec->state = AUDIT_ENTRY_MISSING_WORKTREE;
				out->missing_worktree_count++;
			} else {
				rec->state = AUDIT_ENTRY_NORMAL;
				out->normal_count++;
			}
		}
	}

	/*
	 * Case-folding collision diagnostic: paths that differ only by
	 * case collide on case-insensitive filesystems.  This is reported
	 * independently and must not reclassify any entry.
	 */
	for (i = 0; i < count; i++) {
		for (j = i + 1; j < count; j++) {
			const char *a = out->records[i].path;
			const char *b = out->records[j].path;

			if (strcmp(a, b) != 0 && git__strcasecmp(a, b) == 0) {
				git_str_printf(&out->case_collisions, "%s <-> %s\n", a, b);
				out->case_collision_count++;
			}
		}
	}

	out->committable = (out->conflict_entry_count == 0);

done:
	git_str_dispose(&fullpath);
	return error;
}

static const audit_record *find_record(const audit_report *report, const char *path, int stage)
{
	size_t i;

	for (i = 0; i < report->record_count; i++) {
		const audit_record *rec = &report->records[i];

		if (strcmp(rec->path, path) == 0 && (stage < 0 || rec->stage == stage))
			return rec;
	}

	return NULL;
}

/* --- test fixture ----------------------------------------------------- */

static git_repository *g_repo;
static git_index *g_index;
static git_odb *g_odb;

static git_oid g_normal_oid, g_missing_oid;
static git_oid g_ita_oid, g_base_oid, g_ours_oid, g_theirs_oid;

void test_index_audit__initialize(void)
{
	g_repo = cl_git_sandbox_init("empty_standard_repo");
	cl_git_pass(git_repository_index(&g_index, g_repo));
	cl_git_pass(git_repository_odb(&g_odb, g_repo));
}

void test_index_audit__cleanup(void)
{
	git_odb_free(g_odb);
	g_odb = NULL;

	git_index_free(g_index);
	g_index = NULL;

	cl_git_sandbox_cleanup();
}

static void write_blob(git_oid *out, const char *content)
{
	cl_git_pass(git_odb_write(out, g_odb, content, strlen(content), GIT_OBJECT_BLOB));
}

static void add_entry_at_stage(const char *path, int stage, const git_oid *id)
{
	git_index_entry entry;

	memset(&entry, 0, sizeof(entry));
	entry.mode = GIT_FILEMODE_BLOB;
	git_oid_cpy(&entry.id, id);
	entry.path = path;
	GIT_INDEX_ENTRY_STAGE_SET(&entry, stage);

	cl_git_pass(git_index_add(g_index, &entry));
}

static void add_intent_to_add_entry(const char *path, const git_oid *id)
{
	git_index_entry entry;

	memset(&entry, 0, sizeof(entry));
	entry.mode = GIT_FILEMODE_BLOB;
	git_oid_cpy(&entry.id, id);
	entry.path = path;
	entry.flags_extended = GIT_INDEX_ENTRY_INTENT_TO_ADD;

	cl_git_pass(git_index_add(g_index, &entry));
}

/*
 * Build a small index containing every audited state:
 *  - normal.txt:   ordinary stage 0 entry, present in the worktree
 *  - ita.txt:      intent-to-add entry, present in the worktree
 *  - conflict.txt: stages 1, 2 and 3 (no worktree file)
 *  - missing.txt:  stage 0 entry whose worktree file was deleted
 */
static void build_mixed_index(void)
{
	const git_index_entry *e;

	cl_git_mkfile("./empty_standard_repo/normal.txt", "normal\n");
	cl_git_pass(git_index_add_bypath(g_index, "normal.txt"));
	e = git_index_get_bypath(g_index, "normal.txt", 0);
	git_oid_cpy(&g_normal_oid, &e->id);

	cl_git_mkfile("./empty_standard_repo/ita.txt", "ita\n");
	write_blob(&g_ita_oid, "");
	add_intent_to_add_entry("ita.txt", &g_ita_oid);

	write_blob(&g_base_oid, "base\n");
	write_blob(&g_ours_oid, "ours\n");
	write_blob(&g_theirs_oid, "theirs\n");
	add_entry_at_stage("conflict.txt", 1, &g_base_oid);
	add_entry_at_stage("conflict.txt", 2, &g_ours_oid);
	add_entry_at_stage("conflict.txt", 3, &g_theirs_oid);

	cl_git_mkfile("./empty_standard_repo/missing.txt", "missing\n");
	cl_git_pass(git_index_add_bypath(g_index, "missing.txt"));
	e = git_index_get_bypath(g_index, "missing.txt", 0);
	git_oid_cpy(&g_missing_oid, &e->id);
	cl_must_pass(p_unlink("./empty_standard_repo/missing.txt"));

	cl_git_pass(git_index_write(g_index));
}

/* --- tests ------------------------------------------------------------ */

void test_index_audit__classifies_mixed_entries(void)
{
	audit_report report;
	const audit_record *rec;

	build_mixed_index();

	cl_git_pass(audit_index(g_repo, g_index, &report));

	cl_assert_equal_sz(6, report.record_count);
	cl_assert_equal_sz(1, report.normal_count);
	cl_assert_equal_sz(1, report.intent_to_add_count);
	cl_assert_equal_sz(3, report.conflict_entry_count);
	cl_assert_equal_sz(1, report.conflicted_path_count);
	cl_assert_equal_sz(1, report.missing_worktree_count);
	cl_assert_equal_sz(0, report.case_collision_count);

	/* conflicts make the index uncommittable */
	cl_assert(!report.committable);

	/* normal entry: stage 0, full OID preserved */
	rec = find_record(&report, "normal.txt", -1);
	cl_assert(rec != NULL);
	cl_assert_equal_i(AUDIT_ENTRY_NORMAL, rec->state);
	cl_assert_equal_i(0, rec->stage);
	cl_assert_equal_oid(&g_normal_oid, &rec->oid);

	/* intent-to-add: flagged entry, not "normal", not "missing" */
	rec = find_record(&report, "ita.txt", -1);
	cl_assert(rec != NULL);
	cl_assert_equal_i(AUDIT_ENTRY_INTENT_TO_ADD, rec->state);
	cl_assert_equal_i(0, rec->stage);
	cl_assert((rec->flags_extended & GIT_INDEX_ENTRY_INTENT_TO_ADD) != 0);
	cl_assert_equal_oid(&g_ita_oid, &rec->oid);

	/* conflict stages: identity comes from the stage bits, OIDs kept */
	rec = find_record(&report, "conflict.txt", 1);
	cl_assert(rec != NULL);
	cl_assert_equal_i(AUDIT_ENTRY_CONFLICT, rec->state);
	cl_assert_equal_i(1, (rec->flags & GIT_INDEX_ENTRY_STAGEMASK) >> GIT_INDEX_ENTRY_STAGESHIFT);
	cl_assert_equal_oid(&g_base_oid, &rec->oid);

	rec = find_record(&report, "conflict.txt", 2);
	cl_assert(rec != NULL);
	cl_assert_equal_i(AUDIT_ENTRY_CONFLICT, rec->state);
	cl_assert_equal_i(2, (rec->flags & GIT_INDEX_ENTRY_STAGEMASK) >> GIT_INDEX_ENTRY_STAGESHIFT);
	cl_assert_equal_oid(&g_ours_oid, &rec->oid);

	rec = find_record(&report, "conflict.txt", 3);
	cl_assert(rec != NULL);
	cl_assert_equal_i(AUDIT_ENTRY_CONFLICT, rec->state);
	cl_assert_equal_i(3, (rec->flags & GIT_INDEX_ENTRY_STAGEMASK) >> GIT_INDEX_ENTRY_STAGESHIFT);
	cl_assert_equal_oid(&g_theirs_oid, &rec->oid);

	/* missing worktree file: staged content is still reported */
	rec = find_record(&report, "missing.txt", -1);
	cl_assert(rec != NULL);
	cl_assert_equal_i(AUDIT_ENTRY_MISSING_WORKTREE, rec->state);
	cl_assert_equal_i(0, rec->stage);
	cl_assert_equal_oid(&g_missing_oid, &rec->oid);

	audit_report_dispose(&report);
}

void test_index_audit__does_not_modify_index(void)
{
	audit_report report;
	git_str before = GIT_STR_INIT, after = GIT_STR_INIT;
	git_str index_path = GIT_STR_INIT;

	build_mixed_index();

	cl_git_pass(git_str_joinpath(&index_path,
		git_repository_path(g_repo), "index"));
	cl_git_pass(git_futils_readbuffer(&before, index_path.ptr));

	cl_git_pass(audit_index(g_repo, g_index, &report));
	audit_report_dispose(&report);

	/* the index file on disk is byte-for-byte identical */
	cl_git_pass(git_futils_readbuffer(&after, index_path.ptr));
	cl_assert_equal_sz(before.size, after.size);
	cl_assert(memcmp(before.ptr, after.ptr, before.size) == 0);

	/* and the in-memory index is untouched as well */
	cl_assert_equal_sz(6, git_index_entrycount(g_index));
	cl_assert(git_index_get_bypath(g_index, "missing.txt", 0) != NULL);
	cl_assert(git_index_get_bypath(g_index, "conflict.txt", 2) != NULL);

	git_str_dispose(&index_path);
	git_str_dispose(&before);
	git_str_dispose(&after);
}

/*
 * Clarification: a file missing from the worktree is NOT the same as
 * the staged content being deleted.  The audit must report the entry
 * as "missing worktree content" while keeping its staged OID and
 * flags, and must not drop or alter the index entry (which an index
 * refresh would do).
 */
void test_index_audit__missing_file_is_not_a_staged_removal(void)
{
	audit_report report;
	const audit_record *rec;
	const git_index_entry *entry;
	git_oid staged_oid;
	uint16_t staged_flags;

	cl_git_mkfile("./empty_standard_repo/gone.txt", "staged content\n");
	cl_git_pass(git_index_add_bypath(g_index, "gone.txt"));

	entry = git_index_get_bypath(g_index, "gone.txt", 0);
	cl_assert(entry != NULL);
	git_oid_cpy(&staged_oid, &entry->id);
	staged_flags = entry->flags;

	cl_git_pass(git_index_write(g_index));
	cl_must_pass(p_unlink("./empty_standard_repo/gone.txt"));

	cl_git_pass(audit_index(g_repo, g_index, &report));

	/* reported as missing worktree content, with staged OID/flags kept */
	cl_assert_equal_sz(1, report.missing_worktree_count);
	cl_assert_equal_sz(0, report.normal_count);
	rec = find_record(&report, "gone.txt", -1);
	cl_assert(rec != NULL);
	cl_assert_equal_i(AUDIT_ENTRY_MISSING_WORKTREE, rec->state);
	cl_assert_equal_oid(&staged_oid, &rec->oid);
	cl_assert_equal_i(staged_flags, rec->flags);

	/* no conflicts: still committable; the staged blob would be committed */
	cl_assert(report.committable);

	audit_report_dispose(&report);

	/* the audit did not drop the staged entry from the index */
	cl_assert_equal_sz(1, git_index_entrycount(g_index));
	entry = git_index_get_bypath(g_index, "gone.txt", 0);
	cl_assert(entry != NULL);
	cl_assert_equal_oid(&staged_oid, &entry->id);
}

/*
 * Paths that differ only by case collide on case-insensitive
 * filesystems.  That collision is reported as an independent
 * diagnostic only; each entry's classification and stage identity
 * still come from its own flags.
 */
void test_index_audit__case_collision_is_diagnostic_only(void)
{
	audit_report report;
	const audit_record *rec;

	write_blob(&g_ours_oid, "ours\n");
	write_blob(&g_theirs_oid, "theirs\n");
	add_entry_at_stage("CaseFile.txt", 2, &g_ours_oid);
	add_entry_at_stage("casefile.txt", 3, &g_theirs_oid);

	cl_git_mkfile("./empty_standard_repo/plain.txt", "plain\n");
	cl_git_pass(git_index_add_bypath(g_index, "plain.txt"));

	cl_git_pass(git_index_write(g_index));

	cl_git_pass(audit_index(g_repo, g_index, &report));

	/* real stage identity is preserved for both colliding paths */
	rec = find_record(&report, "CaseFile.txt", -1);
	cl_assert(rec != NULL);
	cl_assert_equal_i(AUDIT_ENTRY_CONFLICT, rec->state);
	cl_assert_equal_i(2, rec->stage);
	cl_assert_equal_oid(&g_ours_oid, &rec->oid);

	rec = find_record(&report, "casefile.txt", -1);
	cl_assert(rec != NULL);
	cl_assert_equal_i(AUDIT_ENTRY_CONFLICT, rec->state);
	cl_assert_equal_i(3, rec->stage);
	cl_assert_equal_oid(&g_theirs_oid, &rec->oid);

	/* the normal entry is unaffected by the collision diagnostic */
	rec = find_record(&report, "plain.txt", -1);
	cl_assert(rec != NULL);
	cl_assert_equal_i(AUDIT_ENTRY_NORMAL, rec->state);

	/* the collision shows up exactly once, as a diagnostic */
	cl_assert_equal_sz(1, report.case_collision_count);
	cl_assert(report.case_collisions.ptr != NULL);
	cl_assert(strstr(report.case_collisions.ptr,
		"CaseFile.txt <-> casefile.txt") != NULL);

	audit_report_dispose(&report);
}
