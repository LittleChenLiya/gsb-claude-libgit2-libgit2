# Read-only index audit (`lg2 audit-index`)

`audit-index` is a read-only audit of the repository index.  It walks every
index entry and classifies it by its real stage and flags, then prints a
committable-status summary.  It never writes the index or the worktree.

## Why read-only

A common mistake when inspecting the index is to "help" it along: refresh
from disk, re-hash a missing file, or re-add an entry that looks out of
date.  Any of those can rewrite the index (and drop or alter entries) or
touch the worktree.  `audit-index` does neither.  It opens the index once
and only reads: `git_index_entrycount`, `git_index_get_byindex`,
`git_index_entry_stage`, the entry's `flags`/`flags_extended`, and a
`stat` of the worktree file.  It never calls `git_index_read(force)`,
`git_index_write`, `git_index_add*` or `git_index_remove*`, and never
creates, modifies or deletes a worktree file.

## Classifications

Every record is assigned exactly one class, in this precedence:

1. `conflict` — the entry has stage 1, 2 or 3 (ancestor / ours / theirs).
   A conflicted path blocks a commit until resolved.
2. `intent-to-add` — stage 0 carrying `GIT_INDEX_ENTRY_INTENT_TO_ADD`.
   The entry reserves the path but holds no staged content (zero OID).
3. `missing-worktree` — stage 0 with a valid staged OID whose worktree
   file is absent.  **This is not a deletion**: the staged OID is still
   in the index and the blob is still in the object database; the content
   is committable, only the worktree copy is missing.
4. `normal` — stage 0 with staged content and the worktree file present.

Each record prints the full 40-hex OID and the raw `flags` and
`flags_extended` values so the on-disk identity is preserved verbatim.

## Case collisions

On case-insensitive platforms (Windows, macOS) two entries that differ
only in case (e.g. `Foo.txt` and `foo.txt`) fold to one name.  The audit
scans for such pairs and reports them as an **independent diagnostic**.
A collision never changes an entry's class, never merges records and
never drops them; the real stage identity always wins.

## Summary

The summary reports counts per class, the number of distinct unmerged
(conflicted) paths, the number of case collisions, and a `committable`
verdict.  The index is committable when there are no unmerged paths.
Intent-to-add entries contribute no staged content; missing-worktree
entries keep their staged OID and remain committable.

## Usage

    lg2 audit-index [--json] [<repo-dir>]

`--json` emits machine-readable records, collisions and summary.

## Acceptance

The clar suite `index::audit` (`tests/libgit2/index/audit.c`) covers:

* classification of normal, intent-to-add, three-stage conflict and
  missing-worktree entries in one fixture;
* index bytes identical before and after the audit;
* a missing worktree file does not remove the staged entry or its blob;
* a case-folded collision is reported as a diagnostic only, with both
  records keeping stage 0 and their own OID;
* the audit does not refresh the index (a second handle's write is not
  picked up) and does not modify a worktree file.

Run the targeted suite single-threaded with:

    ./libgit2_tests -v -sindex::audit
