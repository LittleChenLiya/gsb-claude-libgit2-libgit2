#include "clar_libgit2.h"
#include "git2/repository.h"
#include "git2/index.h"
#include "git2/odb.h"
#include "futils.h"

/*
 * Read-only index audit tests.
 *
 * These mirror the classification performed by the "audit-index"
 * example: every entry keeps its real stage, OID and flags; intent-to-add
 * and conflict stages are reported on their own; a missing worktree file
 * is never confused with deletion of staged content; case-folded path
 * collisions are an independent diagnostic only.
 *
 * The audit under test is read-only: it opens the index once and never
 * refreshes, writes or removes index entries, and never touches the
 * worktree.
 */

static git_repository *repo;
static git_odb *odb;
static git_index *index;

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
	git_oid oid;
	int worktree_present;
	audit_class klass;
} audit_record;

void test_index_audit__initialize(void)
{
	repo = cl_git_sandbox_init("empty_standard_repo");
	cl_git_pass(git_repository_odb(&odb, repo));
	cl_git_pass(git_repository_index(&index, repo));
}

void test_index_audit__cleanup(void)
{
	git_index_free(index);
	git_odb_free(odb);
	cl_git_sandbox_cleanup();
}

static int worktree_file_present(git_repository *r, const char *path)
{
	const char *workdir = git_repository_workdir(r);
	char full[GIT_PATH_MAX];
	struct stat st;

	if (!workdir || git_repository_is_bare(r))
		return -1;

	if (snprintf(full, sizeof(full), "%s%s", workdir, path) >= (int)sizeof(full))
		return 0;

	return (stat(full, &st) == 0 && S_ISREG(st.st_mode));
}

static audit_class classify_entry(git_repository *r,
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
		*worktree_present = worktree_file_present(r, entry->path);
		return AUDIT_INTENT_TO_ADD;
	}

	*worktree_present = worktree_file_present(r, entry->path);
	if (*worktree_present == 0)
		return AUDIT_MISSING_WORKTREE;

	return AUDIT_NORMAL;
}

/*
 * Run the read-only audit over the given index.  This only reads:
 * entrycount, get_byindex, stage, flags, oid and worktree stats.
 */
static size_t run_audit(git_index *idx, audit_record **out_records)
{
	size_t i, count;
	audit_record *records;

	count = git_index_entrycount(idx);
	records = calloc(count ? count : 1, sizeof(audit_record));
	cl_assert(records);

	for (i = 0; i < count; i++) {
		const git_index_entry *entry = git_index_get_byindex(idx, i);
		audit_record *record = &records[i];

		record->path = strdup(entry->path);
		cl_assert(record->path);
		record->stage = git_index_entry_stage(entry);
		record->flags = entry->flags;
		record->flags_extended = entry->flags_extended;
		git_oid_cpy(&record->oid, &entry->id);
		record->klass = classify_entry(repo, entry, &record->worktree_present);
	}

	*out_records = records;
	return count;
}

static void audit_records_free(audit_record *records, size_t count)
{
	size_t i;
	for (i = 0; i < count; i++)
		free(records[i].path);
	free(records);
}

static void add_stage_entry(git_index *idx, const char *path,
	unsigned int stage, const git_oid *oid, uint16_t flags_extended)
{
	git_index_entry entry;

	memset(&entry, 0, sizeof(entry));
	entry.ctime.seconds = 1234567890;
	entry.mtime.seconds = 1234567890;
	entry.mode = 0100644;
	entry.path = path;
	entry.flags_extended = flags_extended;
	if (oid)
		git_oid_cpy(&entry.id, oid);
	GIT_INDEX_ENTRY_STAGE_SET(&entry, stage);

	cl_git_pass(git_index_add(idx, &entry));
}

/*
 * Simulate `git add -N`: the entry is flagged intent-to-add and holds
 * no content (zero OID).  git_index_add validates that a non-zero OID
 * exists in the ODB, so add with a real blob and then clear the stored
 * entry's OID.
 */
static void add_intent_to_add(git_index *idx, const char *path,
	const git_oid *valid_oid)
{
	const git_index_entry *stored;
	git_oid zero;

	/* intent-to-add entries hold no content: a null SHA1 OID */
	cl_git_pass(git_oid_from_string(&zero,
		"0000000000000000000000000000000000000000", GIT_OID_SHA1));

	add_stage_entry(idx, path, 0, valid_oid, GIT_INDEX_ENTRY_INTENT_TO_ADD);

	stored = git_index_get_bypath(idx, path, 0);
	cl_assert(stored);
	git_oid_cpy(&((git_index_entry *)stored)->id, &zero);
}

void test_index_audit__classifies_all_states(void)
{
	git_oid normal_oid, missing_oid, ancestor_oid, our_oid, their_oid, zero_oid;
	audit_record *records;
	size_t count, i;
	size_t n_normal = 0, n_ita = 0, n_conflict = 0, n_missing = 0;
	int stage1 = 0, stage2 = 0, stage3 = 0;

	cl_git_pass(git_odb_write(&normal_oid, odb, "normal content\n", 15, GIT_OBJECT_BLOB));
	cl_git_pass(git_odb_write(&missing_oid, odb, "missing content\n", 16, GIT_OBJECT_BLOB));
	cl_git_pass(git_odb_write(&ancestor_oid, odb, "ancestor\n", 9, GIT_OBJECT_BLOB));
	cl_git_pass(git_odb_write(&our_oid, odb, "ours\n", 5, GIT_OBJECT_BLOB));
	cl_git_pass(git_odb_write(&their_oid, odb, "theirs\n", 7, GIT_OBJECT_BLOB));
	cl_git_pass(git_oid_from_string(&zero_oid,
		"0000000000000000000000000000000000000000", GIT_OID_SHA1));

	/* normal entry with its worktree file */
	add_stage_entry(index, "normal.txt", 0, &normal_oid, 0);
	cl_git_mkfile("./empty_standard_repo/normal.txt", "normal content\n");

	/* intent-to-add entry: flagged, no staged content, file on disk */
	add_intent_to_add(index, "intent.txt", &normal_oid);
	cl_git_mkfile("./empty_standard_repo/intent.txt", "intent content\n");

	/* three-stage conflict on one path */
	add_stage_entry(index, "conflict.txt", 1, &ancestor_oid, 0);
	add_stage_entry(index, "conflict.txt", 2, &our_oid, 0);
	add_stage_entry(index, "conflict.txt", 3, &their_oid, 0);

	/* stage-0 entry whose worktree file is missing */
	add_stage_entry(index, "missing.txt", 0, &missing_oid, 0);
	cl_git_mkfile("./empty_standard_repo/missing.txt", "missing content\n");
	cl_git_rmfile("./empty_standard_repo/missing.txt");

	cl_git_pass(git_index_write(index));

	count = run_audit(index, &records);
	cl_assert_equal_i(count, 6);

	for (i = 0; i < count; i++) {
		audit_record *r = &records[i];
		switch (r->klass) {
		case AUDIT_NORMAL:
			n_normal++;
			break;
		case AUDIT_INTENT_TO_ADD:
			n_ita++;
			break;
		case AUDIT_CONFLICT:
			n_conflict++;
			if (!strcmp(r->path, "conflict.txt")) {
				if (r->stage == 1) stage1 = 1;
				if (r->stage == 2) stage2 = 1;
				if (r->stage == 3) stage3 = 1;
			}
			break;
		case AUDIT_MISSING_WORKTREE:
			n_missing++;
			break;
		}
	}

	cl_assert_equal_i(n_normal, 1);
	cl_assert_equal_i(n_ita, 1);
	cl_assert_equal_i(n_conflict, 3);
	cl_assert_equal_i(n_missing, 1);
	cl_assert(stage1 && stage2 && stage3);

	/* every record keeps its real OID and flags */
	for (i = 0; i < count; i++) {
		audit_record *r = &records[i];
		if (!strcmp(r->path, "normal.txt")) {
			cl_assert_equal_i(r->stage, 0);
			cl_assert(!git_oid_cmp(&r->oid, &normal_oid));
			cl_assert(r->worktree_present == 1);
			cl_assert_equal_i(r->klass, AUDIT_NORMAL);
		} else if (!strcmp(r->path, "intent.txt")) {
			cl_assert_equal_i(r->stage, 0);
			cl_assert(r->flags_extended & GIT_INDEX_ENTRY_INTENT_TO_ADD);
			cl_assert(!git_oid_cmp(&r->oid, &zero_oid));
			cl_assert_equal_i(r->klass, AUDIT_INTENT_TO_ADD);
		} else if (!strcmp(r->path, "conflict.txt")) {
			cl_assert(r->stage >= 1 && r->stage <= 3);
			cl_assert_equal_i(r->klass, AUDIT_CONFLICT);
		} else if (!strcmp(r->path, "missing.txt")) {
			cl_assert_equal_i(r->stage, 0);
			cl_assert(!git_oid_cmp(&r->oid, &missing_oid));
			cl_assert(r->worktree_present == 0);
			cl_assert_equal_i(r->klass, AUDIT_MISSING_WORKTREE);
		}
	}

	audit_records_free(records, count);
}

void test_index_audit__index_bytes_unchanged(void)
{
	git_oid oid;
	audit_record *records;
	size_t count;
	git_str before = GIT_STR_INIT, after = GIT_STR_INIT;
	const char *index_path = git_index_path(index);

	cl_git_pass(git_odb_write(&oid, odb, "content\n", 8, GIT_OBJECT_BLOB));
	add_stage_entry(index, "file.txt", 0, &oid, 0);
	cl_git_mkfile("./empty_standard_repo/file.txt", "content\n");
	cl_git_pass(git_index_write(index));

	cl_git_pass(git_futils_readbuffer(&before, index_path));

	count = run_audit(index, &records);
	cl_assert_equal_i(count, 1);
	audit_records_free(records, count);

	cl_git_pass(git_futils_readbuffer(&after, index_path));
	cl_assert_equal_i(git_str_cmp(&before, &after), 0);

	git_str_dispose(&before);
	git_str_dispose(&after);
}

void test_index_audit__missing_file_does_not_drop_staged_content(void)
{
	git_oid missing_oid;
	audit_record *records;
	size_t count;
	const git_index_entry *staged;

	cl_git_pass(git_odb_write(&missing_oid, odb, "missing content\n", 16, GIT_OBJECT_BLOB));
	add_stage_entry(index, "missing.txt", 0, &missing_oid, 0);
	cl_git_mkfile("./empty_standard_repo/missing.txt", "missing content\n");
	cl_git_rmfile("./empty_standard_repo/missing.txt");
	cl_git_pass(git_index_write(index));

	count = run_audit(index, &records);
	cl_assert_equal_i(count, 1);

	/* the audit reports the missing worktree file ... */
	cl_assert_equal_i(records[0].klass, AUDIT_MISSING_WORKTREE);
	cl_assert(records[0].worktree_present == 0);

	/* ... but the staged entry and its OID are still in the index */
	staged = git_index_get_bypath(index, "missing.txt", 0);
	cl_assert(staged != NULL);
	cl_assert(!git_oid_cmp(&staged->id, &missing_oid));

	/* ... and the blob is still in the object database */
	cl_assert(git_odb_exists(odb, &missing_oid));

	audit_records_free(records, count);
}

void test_index_audit__case_collision_is_diagnostic_only(void)
{
	git_oid case_oid, lower_oid;
	audit_record *records;
	size_t count, i;
	size_t n_conflict = 0;
	int saw_case = 0, saw_lower = 0;
	git_index *case_index;

	/* a case-sensitive index can hold both folded spellings */
	cl_git_pass(git_index_open(&case_index, git_index_path(index)));

	cl_git_pass(git_odb_write(&case_oid, odb, "upper\n", 6, GIT_OBJECT_BLOB));
	cl_git_pass(git_odb_write(&lower_oid, odb, "lower\n", 6, GIT_OBJECT_BLOB));
	add_stage_entry(case_index, "Case.txt", 0, &case_oid, 0);
	add_stage_entry(case_index, "case.txt", 0, &lower_oid, 0);
	cl_git_pass(git_index_write(case_index));

	/* no worktree files: both entries are stage 0 with valid OIDs */
	count = run_audit(case_index, &records);
	cl_assert_equal_i(count, 2);

	for (i = 0; i < count; i++) {
		audit_record *r = &records[i];
		cl_assert_equal_i(r->stage, 0);
		if (!strcmp(r->path, "Case.txt")) {
			cl_assert(!git_oid_cmp(&r->oid, &case_oid));
			saw_case = 1;
		} else if (!strcmp(r->path, "case.txt")) {
			cl_assert(!git_oid_cmp(&r->oid, &lower_oid));
			saw_lower = 1;
		}
		if (r->klass == AUDIT_CONFLICT)
			n_conflict++;
	}

	/* collision is a diagnostic, not a conflict: identities unchanged */
	cl_assert(saw_case && saw_lower);
	cl_assert_equal_i(n_conflict, 0);

	audit_records_free(records, count);
	git_index_free(case_index);
}

void test_index_audit__does_not_refresh_index(void)
{
	git_oid oid_a, oid_b;
	git_index *on_disk;
	audit_record *records;
	size_t count;

	cl_git_pass(git_odb_write(&oid_a, odb, "a\n", 2, GIT_OBJECT_BLOB));
	add_stage_entry(index, "a.txt", 0, &oid_a, 0);
	cl_git_mkfile("./empty_standard_repo/a.txt", "a\n");
	cl_git_pass(git_index_write(index));

	/*
	 * A second handle on the same index file gains another entry and
	 * writes it to disk.  The cached handle must not pick it up: the
	 * audit never refreshes the index.
	 */
	cl_git_pass(git_index_open(&on_disk, git_index_path(index)));
	cl_git_pass(git_odb_write(&oid_b, odb, "b\n", 2, GIT_OBJECT_BLOB));
	add_stage_entry(on_disk, "b.txt", 0, &oid_b, 0);
	cl_git_pass(git_index_write(on_disk));

	count = run_audit(index, &records);
	cl_assert_equal_i(count, 1);
	cl_assert(!strcmp(records[0].path, "a.txt"));
	audit_records_free(records, count);

	git_index_free(on_disk);
}

void test_index_audit__does_not_modify_worktree(void)
{
	git_oid oid;
	audit_record *records;
	size_t count;
	git_str before = GIT_STR_INIT, after = GIT_STR_INIT;

	cl_git_pass(git_odb_write(&oid, odb, "worktree content\n", 17, GIT_OBJECT_BLOB));
	add_stage_entry(index, "wt.txt", 0, &oid, 0);
	cl_git_mkfile("./empty_standard_repo/wt.txt", "worktree content\n");
	cl_git_pass(git_index_write(index));

	cl_git_pass(git_futils_readbuffer(&before, "./empty_standard_repo/wt.txt"));

	count = run_audit(index, &records);
	cl_assert_equal_i(count, 1);
	cl_assert_equal_i(records[0].klass, AUDIT_NORMAL);
	audit_records_free(records, count);

	cl_git_pass(git_futils_readbuffer(&after, "./empty_standard_repo/wt.txt"));
	cl_assert_equal_i(git_str_cmp(&before, &after), 0);

	git_str_dispose(&before);
	git_str_dispose(&after);
}
