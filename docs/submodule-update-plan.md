# Read-only submodule update planning

The `git_submodule_update_plan` API (see `git2/submodule_plan.h`)
compares the gitlinks and `.gitmodules` of two superproject commits
and describes what a submodule update between them would involve,
without changing anything.

## What the plan contains

Each `git_submodule_plan_entry` describes one submodule, identified
by its name (the name is the stable identity; a submodule that moved
to a different path between the commits keeps its entry and is
flagged `GIT_SUBMODULE_PLAN_ENTRY_MOVED`, not reported as an
add/remove pair).  Entries are flagged as needed with:

* `GIT_SUBMODULE_PLAN_ENTRY_ADDED` / `GIT_SUBMODULE_PLAN_ENTRY_REMOVED`
* `GIT_SUBMODULE_PLAN_ENTRY_MOVED` (same name, different path)
* `GIT_SUBMODULE_PLAN_ENTRY_TARGET_CHANGED` (gitlink OID changed)
* `GIT_SUBMODULE_PLAN_ENTRY_CONFIG_CHANGED`
  (url/branch/update/ignore changed)

For submodules that are initialized locally, the entry additionally
reports whether the new target object exists in the local object
database, whether the submodule's HEAD matches the new target, and
whether its working directory is clean.  A missing target object is
reported as `needs_fetch`; the planner never uses the network and
never fetches.  These local-inspection fields are tri-state: 1 for
true, 0 for false, -1 for unknown (for example when the submodule is
not initialized).

Inconsistent or malicious data in the new commit is reported through
`git_submodule_plan_diagnostic`: invalid paths (absolute paths or
paths that escape the repository, such as `../evil`), duplicate
submodule names, duplicate paths, gitlinks without matching
`.gitmodules` entries, and `.gitmodules` entries without matching
gitlinks.  Entries with invalid paths are never inspected on disk.

## Read-only guarantee

`git_submodule_update_plan_new` does not modify the repository, its
index, its configuration, any submodule, or any working directory,
and it never accesses the network.

## Resource lifecycle

* Create a plan with `git_submodule_update_plan_new()`.
* The plan owns all of its entries, diagnostics and strings.
  Pointers returned by the accessor functions
  (`git_submodule_update_plan_entry_byindex`,
  `git_submodule_update_plan_entry_byname`,
  `git_submodule_update_plan_diagnostic_byindex`) are valid until the
  plan is freed and must not be freed or modified by the caller.
* The plan is self-contained: it holds no repository, submodule or
  other object open, so those may be freed independently.
* Free the plan with `git_submodule_update_plan_free()`.  Freeing
  `NULL` is a no-op.  All pointers obtained from the plan dangle
  after it is freed.
