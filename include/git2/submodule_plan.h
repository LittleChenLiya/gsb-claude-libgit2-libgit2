/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#ifndef INCLUDE_git_submodule_plan_h__
#define INCLUDE_git_submodule_plan_h__

#include "common.h"
#include "types.h"
#include "oid.h"

/**
 * @file git2/submodule_plan.h
 * @brief Read-only submodule update planning
 *
 * A `git_submodule_update_plan` compares the gitlinks and the
 * `.gitmodules` configuration of two commits in a superproject and
 * describes what a submodule update between them would have to do:
 * which submodules are added, removed, moved to a different path,
 * retargeted to a new OID, or had their configuration changed.
 *
 * For submodules that are initialized locally, the plan additionally
 * inspects (read-only) whether the new target object exists in the
 * local object database, whether the submodule HEAD already matches
 * the new target, and whether its working directory is clean.  When
 * the target object is missing locally, the entry is flagged with
 * `needs_fetch`; the planner never accesses the network and never
 * modifies the superproject, any submodule, the index, or the
 * working directory.
 *
 * ## Resource lifecycle
 *
 * - A plan is created with `git_submodule_update_plan_new()`, which
 *   allocates the plan and all of its entries, diagnostics and
 *   strings in one go.
 * - The plan is immutable: all accessor functions return pointers to
 *   memory owned by the plan.  The returned `git_submodule_plan_entry`
 *   and `git_submodule_plan_diagnostic` pointers, and every string
 *   reachable from them (including `name`, `old_path`, `new_path`,
 *   `path` and `message`), remain valid until the plan is freed.
 *   Callers must not free or modify them.
 * - The plan is fully self-contained; it does not hold the
 *   repository, any submodule, or any other object open, so those
 *   may be freed independently once the plan has been created.
 * - Free the plan with `git_submodule_update_plan_free()`.  Freeing
 *   a NULL plan is a no-op.  After freeing, all pointers previously
 *   obtained from the plan are dangling and must not be used.
 *
 * @defgroup git_submodule_plan Read-only submodule update planning
 * @ingroup Git
 * @{
 */
GIT_BEGIN_DECL

/**
 * Flags describing what would change for a single submodule when
 * updating the superproject from the old commit to the new commit.
 */
typedef enum {
	/** The submodule does not exist in the old commit. */
	GIT_SUBMODULE_PLAN_ENTRY_ADDED          = (1u << 0),
	/** The submodule does not exist in the new commit. */
	GIT_SUBMODULE_PLAN_ENTRY_REMOVED        = (1u << 1),
	/** Same submodule name, but the path changed. */
	GIT_SUBMODULE_PLAN_ENTRY_MOVED          = (1u << 2),
	/** The gitlink target OID changed between the commits. */
	GIT_SUBMODULE_PLAN_ENTRY_TARGET_CHANGED = (1u << 3),
	/** url/branch/update/ignore configuration changed. */
	GIT_SUBMODULE_PLAN_ENTRY_CONFIG_CHANGED = (1u << 4)
} git_submodule_plan_entry_flag_t;

/**
 * A single submodule planned update.
 *
 * All strings are owned by the enclosing plan and must not be freed.
 * The local inspection fields use tri-state semantics: 1 for true,
 * 0 for false, and -1 when the value is unknown (for example because
 * the submodule is not initialized locally, has no gitlink in the
 * new commit, or its configured path is invalid).
 */
typedef struct {
	/** Submodule name (its stable identity across moves). */
	const char *name;
	/** Path in the old commit, or NULL if the submodule was added. */
	const char *old_path;
	/** Path in the new commit, or NULL if the submodule was removed. */
	const char *new_path;
	/** Gitlink target in the old commit; zero OID if none. */
	git_oid old_target;
	/** Gitlink target in the new commit; zero OID if none. */
	git_oid new_target;
	/** Bitmask of `git_submodule_plan_entry_flag_t` values. */
	unsigned int flags;

	/** 1 if a local clone for this submodule name exists. */
	int initialized;
	/** 1 if the submodule HEAD equals `new_target`; -1 if unknown. */
	int head_matches_target;
	/** 1 if the submodule working directory is clean; -1 if unknown. */
	int workdir_clean;
	/** 1 if `new_target` exists in the local object store; -1 if unknown. */
	int target_object_exists;
	/**
	 * 1 if the submodule is initialized but `new_target` is missing
	 * from the local object store and would have to be fetched.
	 * The planner never fetches; it only reports.
	 */
	int needs_fetch;
} git_submodule_plan_entry;

/**
 * Kinds of consistency problems the planner can diagnose in the
 * gitlinks and `.gitmodules` data of the new ("target") commit.
 */
typedef enum {
	/** A submodule path is absolute or escapes the repository. */
	GIT_SUBMODULE_PLAN_DIAG_INVALID_PATH = 1,
	/** The same submodule name is configured more than once. */
	GIT_SUBMODULE_PLAN_DIAG_DUPLICATE_NAME,
	/** Two submodule names map to the same path. */
	GIT_SUBMODULE_PLAN_DIAG_DUPLICATE_PATH,
	/** A gitlink exists at a path with no matching configuration. */
	GIT_SUBMODULE_PLAN_DIAG_GITLINK_WITHOUT_CONFIG,
	/** Configuration exists for a path with no matching gitlink. */
	GIT_SUBMODULE_PLAN_DIAG_CONFIG_WITHOUT_GITLINK
} git_submodule_plan_diagnostic_type_t;

/**
 * A single diagnostic about inconsistent or malicious submodule data.
 *
 * All strings are owned by the enclosing plan and must not be freed.
 */
typedef struct {
	/** The kind of problem that was found. */
	git_submodule_plan_diagnostic_type_t type;
	/** The submodule name involved, or NULL if not applicable. */
	const char *name;
	/** The path involved, or NULL if not applicable. */
	const char *path;
	/** A human-readable description of the problem. */
	const char *message;
} git_submodule_plan_diagnostic;

/**
 * Create a read-only submodule update plan between two commits.
 *
 * This compares the gitlinks and `.gitmodules` of `old_commit_id` and
 * `new_commit_id` in `repo` and inspects the local state of already
 * initialized submodules.  The function is strictly read-only: it
 * does not modify the repository, its index, its configuration, any
 * submodule, or the working directory, and it never uses the network.
 *
 * A NULL `old_commit_id` plans an update from the empty tree (every
 * submodule in the new commit is reported as added); a NULL
 * `new_commit_id` plans an update to the empty tree (every submodule
 * in the old commit is reported as removed).
 *
 * @param out the newly allocated plan; free with
 *        `git_submodule_update_plan_free`
 * @param repo the superproject repository
 * @param old_commit_id the commit to update from, or NULL
 * @param new_commit_id the commit to update to, or NULL
 * @return 0 on success, or an error code
 */
GIT_EXTERN(int) git_submodule_update_plan_new(
	git_submodule_update_plan **out,
	git_repository *repo,
	const git_oid *old_commit_id,
	const git_oid *new_commit_id);

/**
 * Get the number of entries in a submodule update plan.
 *
 * @param plan the plan
 * @return the number of entries
 */
GIT_EXTERN(size_t) git_submodule_update_plan_entry_count(
	const git_submodule_update_plan *plan);

/**
 * Get a plan entry by index.  Entries are sorted by submodule name.
 *
 * The returned pointer is owned by the plan and must not be freed;
 * it is valid until `git_submodule_update_plan_free` is called.
 *
 * @param plan the plan
 * @param idx the index of the entry
 * @return the entry, or NULL if the index is out of range
 */
GIT_EXTERN(const git_submodule_plan_entry *) git_submodule_update_plan_entry_byindex(
	const git_submodule_update_plan *plan,
	size_t idx);

/**
 * Get a plan entry by submodule name.
 *
 * The returned pointer is owned by the plan and must not be freed;
 * it is valid until `git_submodule_update_plan_free` is called.
 *
 * @param plan the plan
 * @param name the submodule name to look up
 * @return the entry, or NULL if no entry exists for that name
 */
GIT_EXTERN(const git_submodule_plan_entry *) git_submodule_update_plan_entry_byname(
	const git_submodule_update_plan *plan,
	const char *name);

/**
 * Get the number of diagnostics in a submodule update plan.
 *
 * @param plan the plan
 * @return the number of diagnostics
 */
GIT_EXTERN(size_t) git_submodule_update_plan_diagnostic_count(
	const git_submodule_update_plan *plan);

/**
 * Get a plan diagnostic by index.
 *
 * The returned pointer is owned by the plan and must not be freed;
 * it is valid until `git_submodule_update_plan_free` is called.
 *
 * @param plan the plan
 * @param idx the index of the diagnostic
 * @return the diagnostic, or NULL if the index is out of range
 */
GIT_EXTERN(const git_submodule_plan_diagnostic *) git_submodule_update_plan_diagnostic_byindex(
	const git_submodule_update_plan *plan,
	size_t idx);

/**
 * Free a submodule update plan and all of its entries, diagnostics
 * and strings.  All pointers previously obtained from the plan
 * become invalid.  Freeing a NULL plan is a no-op.
 *
 * @param plan the plan to free, or NULL
 */
GIT_EXTERN(void) git_submodule_update_plan_free(
	git_submodule_update_plan *plan);

/** @} */
GIT_END_DECL
#endif
