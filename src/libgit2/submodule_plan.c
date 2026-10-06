/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "common.h"

#include "git2/submodule_plan.h"
#include "git2/commit.h"
#include "git2/tree.h"
#include "git2/blob.h"
#include "git2/config.h"
#include "git2/sys/config.h"
#include "git2/repository.h"
#include "git2/odb.h"
#include "git2/refs.h"
#include "git2/status.h"

#include "vector.h"
#include "str.h"
#include "config_backend.h"
#include "fs_path.h"
#include "path.h"

#define GIT_MODULES_FILE ".gitmodules"

/*
 * A read-only submodule update planner.
 *
 * This compares the gitlinks and .gitmodules of two superproject
 * commits and describes the resulting submodule changes.  Nothing in
 * this file writes to the repository, the index, the configuration,
 * any submodule, or the working directory, and nothing touches the
 * network.
 */

typedef struct {
	char *path;
	git_oid oid;
} plan_gitlink;

typedef struct {
	char *name;
	char *path;
	char *url;
	char *branch;
	char *update;
	char *ignore;
	bool path_valid;
} plan_config;

typedef struct {
	git_vector gitlinks; /* plan_gitlink *, insertion order */
	git_vector configs;  /* plan_config *, insertion order */
} plan_snapshot;

struct git_submodule_update_plan {
	git_vector entries;     /* git_submodule_plan_entry *, sorted by name */
	git_vector diagnostics; /* git_submodule_plan_diagnostic * */
};

static void plan_gitlink_free(plan_gitlink *link)
{
	if (!link)
		return;
	git__free(link->path);
	git__free(link);
}

static void plan_config_free(plan_config *cfg)
{
	if (!cfg)
		return;
	git__free(cfg->name);
	git__free(cfg->path);
	git__free(cfg->url);
	git__free(cfg->branch);
	git__free(cfg->update);
	git__free(cfg->ignore);
	git__free(cfg);
}

static void plan_snapshot_dispose(plan_snapshot *snap)
{
	size_t i;
	plan_gitlink *link;
	plan_config *cfg;

	git_vector_foreach(&snap->gitlinks, i, link)
		plan_gitlink_free(link);
	git_vector_foreach(&snap->configs, i, cfg)
		plan_config_free(cfg);

	git_vector_dispose(&snap->gitlinks);
	git_vector_dispose(&snap->configs);
}

static int plan_entry_cmp(const void *a, const void *b)
{
	const git_submodule_plan_entry *ea = a, *eb = b;
	return strcmp(ea->name, eb->name);
}

static int plan_add_diagnostic(
	git_vector *diags,
	git_submodule_plan_diagnostic_type_t type,
	const char *name,
	const char *path,
	const char *fmt,
	...)
{
	git_submodule_plan_diagnostic *diag;
	git_str message = GIT_STR_INIT;
	va_list ap;
	int error = -1;

	diag = git__calloc(1, sizeof(git_submodule_plan_diagnostic));
	GIT_ERROR_CHECK_ALLOC(diag);

	diag->type = type;

	if (name && (diag->name = git__strdup(name)) == NULL)
		goto on_error;
	if (path && (diag->path = git__strdup(path)) == NULL)
		goto on_error;

	va_start(ap, fmt);
	git_str_vprintf(&message, fmt, ap);
	va_end(ap);

	if (git_str_oom(&message))
		goto on_error;

	diag->message = git_str_detach(&message);

	if ((error = git_vector_insert(diags, diag)) < 0)
		goto on_error;

	return 0;

on_error:
	git__free((char *)diag->name);
	git__free((char *)diag->path);
	git__free((char *)diag->message);
	git__free(diag);
	git_str_dispose(&message);
	return error;
}

static plan_config *plan_snapshot_find_config(plan_snapshot *snap, const char *name)
{
	size_t i;
	plan_config *cfg;

	git_vector_foreach(&snap->configs, i, cfg) {
		if (!strcmp(cfg->name, name))
			return cfg;
	}

	return NULL;
}

static const plan_gitlink *plan_snapshot_find_gitlink(
	const plan_snapshot *snap, const char *path)
{
	size_t i;
	plan_gitlink *link;

	git_vector_foreach(&snap->gitlinks, i, link) {
		if (!strcmp(link->path, path))
			return link;
	}

	return NULL;
}

/*
 * Validate a submodule path from .gitmodules: it must be relative and
 * must not escape the repository (eg "../evil") or use otherwise
 * unsafe components.
 */
static bool plan_path_is_valid(git_repository *repo, const char *path)
{
	git_str normalized = GIT_STR_INIT;
	bool valid;

	if (!path || !*path || git_fs_path_is_absolute(path))
		return false;

	if (strchr(path, '\\') != NULL) {
		if (git_fs_path_normalize_slashes(&normalized, path) < 0) {
			git_error_clear();
			return false;
		}
	} else {
		git_str_attach_notowned(&normalized, path, strlen(path));
	}

	valid = git_path_is_valid(
		repo, normalized.ptr, 0, GIT_FS_PATH_REJECT_FILESYSTEM_DEFAULTS);

	git_str_dispose(&normalized);
	return valid;
}

typedef struct {
	plan_snapshot *snap;
} plan_treewalk_payload;

static int plan_treewalk_cb(const char *root, const git_tree_entry *entry, void *payload)
{
	plan_treewalk_payload *wp = payload;
	plan_gitlink *link;
	git_str path = GIT_STR_INIT;

	if (git_tree_entry_filemode(entry) != GIT_FILEMODE_COMMIT)
		return 0;

	if (git_str_joinpath(&path, root, git_tree_entry_name(entry)) < 0)
		return -1;

	link = git__calloc(1, sizeof(plan_gitlink));
	GIT_ERROR_CHECK_ALLOC(link);

	link->path = git_str_detach(&path);
	git_oid_cpy(&link->oid, git_tree_entry_id(entry));

	if (git_vector_insert(&wp->snap->gitlinks, link) < 0) {
		plan_gitlink_free(link);
		return -1;
	}

	return 0;
}

static int plan_config_upsert_value(
	plan_snapshot *snap,
	git_vector *diags,
	const char *name,
	const char *key,
	const char *value)
{
	plan_config *cfg = plan_snapshot_find_config(snap, name);
	char **slot = NULL;

	if (!cfg) {
		cfg = git__calloc(1, sizeof(plan_config));
		GIT_ERROR_CHECK_ALLOC(cfg);
		cfg->path_valid = true;

		if ((cfg->name = git__strdup(name)) == NULL ||
		    git_vector_insert(&snap->configs, cfg) < 0) {
			plan_config_free(cfg);
			return -1;
		}
	}

	if (!strcmp(key, "path"))
		slot = &cfg->path;
	else if (!strcmp(key, "url"))
		slot = &cfg->url;
	else if (!strcmp(key, "branch"))
		slot = &cfg->branch;
	else if (!strcmp(key, "update"))
		slot = &cfg->update;
	else if (!strcmp(key, "ignore"))
		slot = &cfg->ignore;
	else
		return 0;

	/*
	 * A repeated "path" for the same name means the submodule was
	 * configured twice; diagnose it and let the last value win,
	 * matching git config semantics.
	 */
	if (slot == &cfg->path && cfg->path != NULL && diags != NULL) {
		if (plan_add_diagnostic(diags,
				GIT_SUBMODULE_PLAN_DIAG_DUPLICATE_NAME,
				name, value,
				"submodule '%s' is configured more than once",
				name) < 0)
			return -1;
	}

	git__free(*slot);
	if ((*slot = git__strdup(value)) == NULL)
		return -1;

	return 0;
}

static int plan_parse_gitmodules(
	plan_snapshot *snap,
	git_repository *repo,
	git_vector *diags,
	const char *content,
	size_t content_len)
{
	git_config_backend *backend = NULL;
	git_config *cfg = NULL;
	git_config_iterator *iter = NULL;
	git_config_entry *entry;
	int error;

	if ((error = git_config_backend_from_string(
			&backend, content, content_len, NULL)) < 0)
		goto out;

	if ((error = git_config_new(&cfg)) < 0)
		goto out;

	if ((error = git_config_add_backend(
			cfg, backend, GIT_CONFIG_LEVEL_LOCAL, repo, 0)) < 0)
		goto out;
	backend = NULL; /* owned by cfg now */

	if ((error = git_config_iterator_new(&iter, cfg)) < 0)
		goto out;

	while ((error = git_config_next(&entry, iter)) == 0) {
		const char *first_dot, *last_dot, *key;
		git_str name = GIT_STR_INIT;

		if (strncmp(entry->name, "submodule.", strlen("submodule.")) != 0)
			continue;

		first_dot = entry->name + strlen("submodule.");
		last_dot = strrchr(first_dot, '.');
		if (!last_dot || last_dot == first_dot)
			continue;
		key = last_dot + 1;

		if (git_str_set(&name, first_dot, last_dot - first_dot) < 0) {
			error = -1;
			break;
		}

		error = plan_config_upsert_value(
			snap, diags, name.ptr, key, entry->value ? entry->value : "");
		git_str_dispose(&name);

		if (error < 0)
			break;
	}

	if (error == GIT_ITEROVER)
		error = 0;

out:
	git_config_iterator_free(iter);
	git_config_free(cfg);
	if (backend)
		git_config_backend_free(backend);
	return error;
}

static int plan_snapshot_load_gitmodules(
	plan_snapshot *snap,
	git_repository *repo,
	git_vector *diags,
	git_tree *tree)
{
	git_tree_entry *modules_entry = NULL;
	git_blob *blob = NULL;
	int error;

	error = git_tree_entry_bypath(&modules_entry, tree, GIT_MODULES_FILE);
	if (error == GIT_ENOTFOUND) {
		git_error_clear();
		return 0;
	} else if (error < 0) {
		return error;
	}

	if (git_tree_entry_type(modules_entry) != GIT_OBJECT_BLOB) {
		error = 0;
		goto out;
	}

	if ((error = git_blob_lookup(&blob, repo, git_tree_entry_id(modules_entry))) < 0)
		goto out;

	error = plan_parse_gitmodules(
		snap, repo, diags,
		git_blob_rawcontent(blob), (size_t)git_blob_rawsize(blob));

out:
	git_blob_free(blob);
	git_tree_entry_free(modules_entry);
	return error;
}

/*
 * Collect the gitlinks and .gitmodules configuration of a commit.
 * Diagnostics are only recorded when `diags` is non-NULL (that is,
 * for the new commit, which is the state an update would move to).
 */
static int plan_snapshot_load(
	plan_snapshot *snap,
	git_repository *repo,
	const git_oid *commit_id,
	git_vector *diags)
{
	git_commit *commit = NULL;
	git_tree *tree = NULL;
	plan_treewalk_payload wp;
	size_t i, j;
	plan_config *cfg;
	plan_gitlink *link;
	int error = 0;

	memset(snap, 0, sizeof(*snap));
	if (git_vector_init(&snap->gitlinks, 0, NULL) < 0 ||
	    git_vector_init(&snap->configs, 0, NULL) < 0)
		return -1;

	if (!commit_id)
		return 0;

	if ((error = git_commit_lookup(&commit, repo, commit_id)) < 0)
		return error;

	if ((error = git_commit_tree(&tree, commit)) < 0)
		goto out;

	wp.snap = snap;
	if ((error = git_tree_walk(tree, GIT_TREEWALK_PRE, plan_treewalk_cb, &wp)) < 0)
		goto out;

	if ((error = plan_snapshot_load_gitmodules(snap, repo, diags, tree)) < 0)
		goto out;

	if (diags == NULL)
		goto out;

	/* validate configured paths and detect duplicates */
	git_vector_foreach(&snap->configs, i, cfg) {
		plan_config *other;

		if (!cfg->path) {
			cfg->path_valid = false;
			continue;
		}

		cfg->path_valid = plan_path_is_valid(repo, cfg->path);
		if (!cfg->path_valid) {
			if ((error = plan_add_diagnostic(diags,
					GIT_SUBMODULE_PLAN_DIAG_INVALID_PATH,
					cfg->name, cfg->path,
					"submodule '%s' has invalid path '%s'",
					cfg->name, cfg->path)) < 0)
				goto out;
			continue;
		}

		for (j = i + 1; j < git_vector_length(&snap->configs); j++) {
			other = git_vector_get(&snap->configs, j);
			if (other->path && !strcmp(other->path, cfg->path)) {
				if ((error = plan_add_diagnostic(diags,
						GIT_SUBMODULE_PLAN_DIAG_DUPLICATE_PATH,
						cfg->name, cfg->path,
						"submodules '%s' and '%s' share path '%s'",
						cfg->name, other->name, cfg->path)) < 0)
					goto out;
			}
		}
	}

	/* diagnose gitlink / configuration mismatches */
	git_vector_foreach(&snap->gitlinks, i, link) {
		bool matched = false;

		git_vector_foreach(&snap->configs, j, cfg) {
			if (cfg->path && !strcmp(cfg->path, link->path)) {
				matched = true;
				break;
			}
		}

		if (!matched && (error = plan_add_diagnostic(diags,
				GIT_SUBMODULE_PLAN_DIAG_GITLINK_WITHOUT_CONFIG,
				NULL, link->path,
				"gitlink at '%s' has no matching .gitmodules entry",
				link->path)) < 0)
			goto out;
	}

	git_vector_foreach(&snap->configs, i, cfg) {
		if (!cfg->path_valid)
			continue;

		if (!plan_snapshot_find_gitlink(snap, cfg->path) &&
		    (error = plan_add_diagnostic(diags,
				GIT_SUBMODULE_PLAN_DIAG_CONFIG_WITHOUT_GITLINK,
				cfg->name, cfg->path,
				"submodule '%s' has no gitlink at '%s'",
				cfg->name, cfg->path)) < 0)
			goto out;
	}

out:
	git_tree_free(tree);
	git_commit_free(commit);
	return error;
}

/*
 * Read-only inspection of the local state of an initialized
 * submodule: does the new target object exist, does HEAD match it,
 * and is the working directory clean.  Missing objects are reported
 * as needs_fetch; nothing is fetched and nothing is written.
 */
static void plan_inspect_local(
	git_repository *repo, git_submodule_plan_entry *entry)
{
	git_config *cfg = NULL;
	git_repository *smrepo = NULL;
	git_str key = GIT_STR_INIT, path = GIT_STR_INIT;
	const char *url = NULL;
	bool has_target;

	entry->initialized = 0;
	entry->head_matches_target = -1;
	entry->workdir_clean = -1;
	entry->target_object_exists = -1;
	entry->needs_fetch = 0;

	if (!entry->new_path)
		return;

	/* a submodule counts as initialized when it has a local url */
	if (git_repository_config_snapshot(&cfg, repo) == 0) {
		if (git_str_printf(&key, "submodule.%s.url", entry->name) == 0 &&
		    git_config_get_string(&url, cfg, key.ptr) == 0)
			entry->initialized = 1;
		else
			git_error_clear();
		git_config_free(cfg);
	} else {
		git_error_clear();
	}

	if (!entry->initialized)
		return;

	/* the local clone lives in .git/modules/<name>, keyed by name so
	 * that it survives moves of the worktree path */
	if (git_str_joinpath(&path, git_repository_path(repo), "modules/") < 0 ||
	    git_str_joinpath(&path, path.ptr, entry->name) < 0)
		goto out;

	if (git_repository_open(&smrepo, path.ptr) < 0) {
		const char *workdir = git_repository_workdir(repo);

		git_error_clear();
		if (!workdir)
			goto out;

		git_str_clear(&path);
		if (git_str_joinpath(&path, workdir, entry->new_path) < 0 ||
		    git_repository_open(&smrepo, path.ptr) < 0) {
			git_error_clear();
			goto out;
		}
	}

	has_target = !git_oid_is_zero(&entry->new_target);

	if (has_target) {
		git_odb *odb = NULL;
		git_oid head;

		if (git_repository_odb(&odb, smrepo) == 0) {
			entry->target_object_exists =
				git_odb_exists(odb, &entry->new_target) ? 1 : 0;
			git_odb_free(odb);
			entry->needs_fetch = !entry->target_object_exists;
		} else {
			git_error_clear();
		}

		if (git_reference_name_to_id(&head, smrepo, "HEAD") == 0)
			entry->head_matches_target =
				git_oid_equal(&head, &entry->new_target) ? 1 : 0;
		else
			git_error_clear();
	}

	if (!git_repository_is_bare(smrepo)) {
		git_status_options opts = GIT_STATUS_OPTIONS_INIT;
		git_status_list *list = NULL;

		opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
			GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;

		if (git_status_list_new(&list, smrepo, &opts) == 0) {
			entry->workdir_clean =
				(git_status_list_entrycount(list) == 0) ? 1 : 0;
			git_status_list_free(list);
		} else {
			git_error_clear();
		}
	}

out:
	git_repository_free(smrepo);
	git_str_dispose(&key);
	git_str_dispose(&path);
}

static bool plan_config_str_changed(const char *a, const char *b)
{
	if (!a)
		a = "";
	if (!b)
		b = "";
	return strcmp(a, b) != 0;
}

static git_submodule_plan_entry *plan_entry_new(const char *name)
{
	git_submodule_plan_entry *entry;

	entry = git__calloc(1, sizeof(git_submodule_plan_entry));
	if (!entry)
		return NULL;

	entry->name = git__strdup(name);
	if (!entry->name) {
		git__free(entry);
		return NULL;
	}

	entry->initialized = 0;
	entry->head_matches_target = -1;
	entry->workdir_clean = -1;
	entry->target_object_exists = -1;

	return entry;
}

static void plan_entry_free(git_submodule_plan_entry *entry)
{
	if (!entry)
		return;
	git__free((char *)entry->name);
	git__free((char *)entry->old_path);
	git__free((char *)entry->new_path);
	git__free(entry);
}

static int plan_compare_snapshots(
	git_submodule_update_plan *plan,
	plan_snapshot *old_snap,
	plan_snapshot *new_snap)
{
	size_t i;
	plan_config *cfg;
	int error = 0;

	/* added and changed submodules */
	git_vector_foreach(&new_snap->configs, i, cfg) {
		plan_config *old_cfg = plan_snapshot_find_config(old_snap, cfg->name);
		const plan_gitlink *new_link = cfg->path ?
			plan_snapshot_find_gitlink(new_snap, cfg->path) : NULL;
		git_submodule_plan_entry *entry = NULL;

		if (!old_cfg) {
			if ((entry = plan_entry_new(cfg->name)) == NULL)
				return -1;
			entry->flags |= GIT_SUBMODULE_PLAN_ENTRY_ADDED;
		} else {
			const plan_gitlink *old_link = old_cfg->path ?
				plan_snapshot_find_gitlink(old_snap, old_cfg->path) : NULL;
			git_oid zero = { 0 };

			if ((entry = plan_entry_new(cfg->name)) == NULL)
				return -1;

			if (old_cfg->path &&
			    (entry->old_path = git__strdup(old_cfg->path)) == NULL) {
				plan_entry_free(entry);
				return -1;
			}
			if (old_link)
				git_oid_cpy(&entry->old_target, &old_link->oid);

			if (!old_cfg->path || !cfg->path ||
			    strcmp(old_cfg->path, cfg->path) != 0)
				entry->flags |= GIT_SUBMODULE_PLAN_ENTRY_MOVED;

			if (!git_oid_equal(&entry->old_target,
				new_link ? &new_link->oid : &zero))
				entry->flags |= GIT_SUBMODULE_PLAN_ENTRY_TARGET_CHANGED;

			if (plan_config_str_changed(old_cfg->url, cfg->url) ||
			    plan_config_str_changed(old_cfg->branch, cfg->branch) ||
			    plan_config_str_changed(old_cfg->update, cfg->update) ||
			    plan_config_str_changed(old_cfg->ignore, cfg->ignore))
				entry->flags |= GIT_SUBMODULE_PLAN_ENTRY_CONFIG_CHANGED;
		}

		if (cfg->path &&
		    (entry->new_path = git__strdup(cfg->path)) == NULL) {
			plan_entry_free(entry);
			return -1;
		}
		if (new_link)
			git_oid_cpy(&entry->new_target, &new_link->oid);

		if ((error = git_vector_insert(&plan->entries, entry)) < 0) {
			plan_entry_free(entry);
			return error;
		}
	}

	/* removed submodules */
	git_vector_foreach(&old_snap->configs, i, cfg) {
		git_submodule_plan_entry *entry;
		const plan_gitlink *old_link;

		if (plan_snapshot_find_config(new_snap, cfg->name))
			continue;

		if ((entry = plan_entry_new(cfg->name)) == NULL)
			return -1;
		entry->flags |= GIT_SUBMODULE_PLAN_ENTRY_REMOVED;

		if (cfg->path &&
		    (entry->old_path = git__strdup(cfg->path)) == NULL) {
			plan_entry_free(entry);
			return -1;
		}

		old_link = cfg->path ?
			plan_snapshot_find_gitlink(old_snap, cfg->path) : NULL;
		if (old_link)
			git_oid_cpy(&entry->old_target, &old_link->oid);

		if ((error = git_vector_insert(&plan->entries, entry)) < 0) {
			plan_entry_free(entry);
			return error;
		}
	}

	return 0;
}

int git_submodule_update_plan_new(
	git_submodule_update_plan **out,
	git_repository *repo,
	const git_oid *old_commit_id,
	const git_oid *new_commit_id)
{
	git_submodule_update_plan *plan = NULL;
	plan_snapshot old_snap = {{0}}, new_snap = {{0}};
	size_t i;
	git_submodule_plan_entry *entry;
	int error;

	GIT_ASSERT_ARG(out);
	GIT_ASSERT_ARG(repo);

	*out = NULL;

	plan = git__calloc(1, sizeof(git_submodule_update_plan));
	GIT_ERROR_CHECK_ALLOC(plan);

	if (git_vector_init(&plan->entries, 0, plan_entry_cmp) < 0 ||
	    git_vector_init(&plan->diagnostics, 0, NULL) < 0) {
		git__free(plan);
		return -1;
	}

	if ((error = plan_snapshot_load(&old_snap, repo, old_commit_id, NULL)) == 0)
		error = plan_snapshot_load(
			&new_snap, repo, new_commit_id, &plan->diagnostics);

	if (error == 0)
		error = plan_compare_snapshots(plan, &old_snap, &new_snap);

	plan_snapshot_dispose(&old_snap);
	plan_snapshot_dispose(&new_snap);

	if (error < 0)
		goto on_error;

	/*
	 * Read-only local inspection of submodules that exist in the new
	 * commit and have a valid path.  Invalid paths (eg "../evil") are
	 * never touched on disk.
	 */
	git_vector_foreach(&plan->entries, i, entry) {
		if (!entry->new_path)
			continue;

		/* re-validate the path defensively before any disk access */
		if (!plan_path_is_valid(repo, entry->new_path))
			continue;

		plan_inspect_local(repo, entry);
	}

	git_vector_sort(&plan->entries);

	*out = plan;
	return 0;

on_error:
	git_submodule_update_plan_free(plan);
	return error;
}

size_t git_submodule_update_plan_entry_count(
	const git_submodule_update_plan *plan)
{
	GIT_ASSERT_ARG_WITH_RETVAL(plan, 0);
	return git_vector_length(&plan->entries);
}

const git_submodule_plan_entry *git_submodule_update_plan_entry_byindex(
	const git_submodule_update_plan *plan,
	size_t idx)
{
	GIT_ASSERT_ARG_WITH_RETVAL(plan, NULL);
	if (idx >= git_vector_length(&plan->entries))
		return NULL;
	return git_vector_get(&plan->entries, idx);
}

const git_submodule_plan_entry *git_submodule_update_plan_entry_byname(
	const git_submodule_update_plan *plan,
	const char *name)
{
	size_t i;
	git_submodule_plan_entry *entry;

	GIT_ASSERT_ARG_WITH_RETVAL(plan, NULL);
	GIT_ASSERT_ARG_WITH_RETVAL(name, NULL);

	git_vector_foreach(&plan->entries, i, entry) {
		if (!strcmp(entry->name, name))
			return entry;
	}

	return NULL;
}

size_t git_submodule_update_plan_diagnostic_count(
	const git_submodule_update_plan *plan)
{
	GIT_ASSERT_ARG_WITH_RETVAL(plan, 0);
	return git_vector_length(&plan->diagnostics);
}

const git_submodule_plan_diagnostic *git_submodule_update_plan_diagnostic_byindex(
	const git_submodule_update_plan *plan,
	size_t idx)
{
	GIT_ASSERT_ARG_WITH_RETVAL(plan, NULL);
	if (idx >= git_vector_length(&plan->diagnostics))
		return NULL;
	return git_vector_get(&plan->diagnostics, idx);
}

void git_submodule_update_plan_free(git_submodule_update_plan *plan)
{
	size_t i;
	git_submodule_plan_entry *entry;
	git_submodule_plan_diagnostic *diag;

	if (!plan)
		return;

	git_vector_foreach(&plan->entries, i, entry)
		plan_entry_free(entry);
	git_vector_dispose(&plan->entries);

	git_vector_foreach(&plan->diagnostics, i, diag) {
		git__free((char *)diag->name);
		git__free((char *)diag->path);
		git__free((char *)diag->message);
		git__free(diag);
	}
	git_vector_dispose(&plan->diagnostics);

	git__free(plan);
}
