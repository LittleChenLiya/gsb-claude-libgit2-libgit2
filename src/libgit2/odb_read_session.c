/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "odb.h"

#include "git2/odb.h"
#include "git2/sys/odb_backend.h"
#include "hashmap_oid.h"
#include "posix.h"

#include <stdlib.h>

/*
 * A read session delivers objects from an ODB subject to a set of
 * caller-selected constraints (allowed types, per-object byte cap, total
 * byte budget) and optionally traverses references along selected edges.
 */

#define GIT_ODB_READ_SESSION_MAX_DEPTH 1024

GIT_HASHMAP_OID_SETUP(git_odb_read_session_seen, bool);

struct git_odb_read_session {
	git_odb *odb;
	unsigned int flags;
	unsigned int allowed_types;
	unsigned int traverse;
	git_object_size_t max_object_size;
	git_object_size_t max_total_size;
	git_object_size_t delivered;
	size_t object_count;
	git_odb_read_session_seen seen;
	git_vector seen_oids;
};

int git_odb_read_session_options_init(
	git_odb_read_session_options *opts,
	unsigned int version)
{
	GIT_ASSERT_ARG(opts);

	GIT_INIT_STRUCTURE_FROM_TEMPLATE(
		opts, version, git_odb_read_session_options,
		GIT_ODB_READ_SESSION_OPTIONS_INIT);
	return 0;
}

int git_odb_read_session_new(
	git_odb_read_session **out,
	git_odb *odb,
	const git_odb_read_session_options *opts)
{
	git_odb_read_session *session;
	git_odb_read_session_options init = GIT_ODB_READ_SESSION_OPTIONS_INIT;

	GIT_ASSERT_ARG(out);
	GIT_ASSERT_ARG(odb);

	*out = NULL;

	if (opts == NULL)
		opts = &init;

	GIT_ERROR_CHECK_VERSION(opts, GIT_ODB_READ_SESSION_OPTIONS_VERSION,
		"git_odb_read_session_options");

	session = git__calloc(1, sizeof(*session));
	GIT_ERROR_CHECK_ALLOC(session);

	session->odb = odb;
	session->flags = opts->flags;
	session->allowed_types = opts->allowed_types;
	session->traverse = opts->traverse;
	session->max_object_size = opts->max_object_size;
	session->max_total_size = opts->max_total_size;

	if (git_vector_init(&session->seen_oids, 0, NULL) < 0) {
		git__free(session);
		return -1;
	}

	*out = session;
	return 0;
}

void git_odb_read_session_free(git_odb_read_session *session)
{
	git_oid *oid;
	size_t i;

	if (session == NULL)
		return;

	git_vector_foreach(&session->seen_oids, i, oid)
		git__free(oid);
	git_vector_dispose(&session->seen_oids);
	git_odb_read_session_seen_dispose(&session->seen);
	git__free(session);
}

/*
 * Deliver a single object through the session.
 *
 * The object is read from the ODB and delivered only if its type is
 * allowed, its size does not exceed the per-object cap, and the session's
 * total budget can cover it.  On failure no object is delivered and no
 * bytes are billed.
 */
static int session_deliver(
	git_odb_object **out,
	git_odb_read_session *session,
	const git_oid *id)
{
	git_odb_object *obj = NULL;
	git_object_t type;
	size_t size;
	int error;

	if (out)
		*out = NULL;

	if ((error = git_odb_read(&obj, session->odb, id)) < 0)
		return error;

	type = git_odb_object_type(obj);
	size = git_odb_object_size(obj);

	/*
	 * Type check: the type comes from the object header and is never
	 * guessed.  A type outside the valid object types is rejected
	 * rather than decoded as an allowed type.
	 */
	if (session->allowed_types != 0) {
		if (type < GIT_OBJECT_COMMIT || type > GIT_OBJECT_TAG ||
		    !(session->allowed_types & (unsigned int)(1 << type))) {
			git_error_set(GIT_ERROR_ODB,
				"object type '%s' is not allowed in this read session",
				git_object_type2string(type));
			error = GIT_EINVALID;
			goto fail;
		}
	}

	/* Per-object byte cap. */
	if (session->max_object_size > 0 &&
	    (git_object_size_t)size > session->max_object_size) {
		git_error_set(GIT_ERROR_ODB,
			"object size %" PRIuZ " exceeds the read session per-object limit %" PRIu64,
			size, session->max_object_size);
		error = GIT_EINVALID;
		goto fail;
	}

	/*
	 * Total byte budget.  Bytes are the actual decompressed content
	 * delivered to the caller, including objects served from the ODB
	 * cache.
	 */
	if (session->max_total_size > 0) {
		if (session->delivered > session->max_total_size ||
		    (git_object_size_t)size > session->max_total_size - session->delivered) {
			git_error_set(GIT_ERROR_ODB,
				"object would exceed the read session total byte budget");
			error = GIT_EINVALID;
			goto fail;
		}
	}

	session->delivered += (git_object_size_t)size;
	session->object_count++;

	if (out)
		*out = obj;
	else
		git_odb_object_free(obj);

	return 0;

fail:
	git_odb_object_free(obj);
	return error;
}

int git_odb_read_session_read(
	git_odb_object **out,
	git_odb_read_session *session,
	const git_oid *id)
{
	GIT_ASSERT_ARG(out);
	GIT_ASSERT_ARG(session);
	GIT_ASSERT_ARG(id);

	return session_deliver(out, session, id);
}

git_object_size_t git_odb_read_session_delivered(git_odb_read_session *session)
{
	GIT_ASSERT_ARG(session);
	return session->delivered;
}

size_t git_odb_read_session_object_count(git_odb_read_session *session)
{
	GIT_ASSERT_ARG(session);
	return session->object_count;
}

/*
 * Parse one entry from a raw tree object.
 *
 * Tree entries are: "<mode> <name>\0<oid>".  The mode is ASCII octal and
 * the oid is raw (20 bytes for SHA1, 32 for SHA256).  Returns 0 on
 * success and advances *buffer_p past the entry.
 */
static int parse_tree_entry(
	const char **buffer_p,
	const char *end,
	git_oid *oid,
	git_object_t *type,
	git_oid_t oid_type)
{
	const char *p = *buffer_p;
	const char *mode_start;
	unsigned long mode;
	char *mode_end;
	size_t oid_size = git_oid_size(oid_type);

	if (oid_size == 0)
		return -1;

	mode_start = p;
	while (p < end && *p >= '0' && *p <= '7')
		p++;
	if (p == mode_start || p >= end || *p != ' ')
		return -1;

	mode = strtoul(mode_start, &mode_end, 8);
	if (mode_end != p)
		return -1;
	p++; /* skip the space after the mode */

	while (p < end && *p != '\0')
		p++;
	if (p >= end)
		return -1;
	p++; /* skip the NUL terminator of the name */

	if ((size_t)(end - p) < oid_size)
		return -1;

	git_oid_from_raw(oid, (const unsigned char *)p, oid_type);
	p += oid_size;

	*buffer_p = p;

	/*
	 * The entry type is derived from the mode, never guessed from the
	 * allowed types.
	 */
	if (S_ISGITLINK(mode))
		*type = GIT_OBJECT_COMMIT;
	else if (S_ISDIR(mode))
		*type = GIT_OBJECT_TREE;
	else
		*type = GIT_OBJECT_BLOB;

	return 0;
}

/* Parse the root tree oid from a raw commit object. */
static int parse_commit_tree(
	git_oid *tree_oid,
	const char *data,
	size_t size,
	git_oid_t oid_type)
{
	const char *p = data;
	const char *end = data + size;
	const char *line_end;
	size_t line_len;

	while (p < end) {
		line_end = memchr(p, '\n', (size_t)(end - p));
		if (line_end == NULL)
			line_end = end;
		line_len = (size_t)(line_end - p);

		if (line_len > 5 && memcmp(p, "tree ", 5) == 0) {
			if (git_oid_from_prefix(tree_oid, p + 5, line_len - 5, oid_type) == 0)
				return 0;
		}

		p = line_end + 1;
	}

	return GIT_ENOTFOUND;
}

/* Parse the target oid from a raw annotated tag object. */
static int parse_tag_target(
	git_oid *target,
	const char *data,
	size_t size,
	git_oid_t oid_type)
{
	const char *p = data;
	const char *end = data + size;
	const char *line_end;
	size_t line_len;

	while (p < end) {
		line_end = memchr(p, '\n', (size_t)(end - p));
		if (line_end == NULL)
			line_end = end;
		line_len = (size_t)(line_end - p);

		if (line_len > 7 && memcmp(p, "object ", 7) == 0) {
			if (git_oid_from_prefix(target, p + 7, line_len - 7, oid_type) == 0)
				return 0;
		}

		p = line_end + 1;
	}

	return GIT_ENOTFOUND;
}

static int traverse_object(
	git_odb_read_session *session,
	const git_oid *id,
	const git_oid *source,
	git_odb_read_session_cb cb,
	void *payload,
	int depth);

static int traverse_tree(
	git_odb_read_session *session,
	git_odb_object *tree_obj,
	const git_oid *tree_oid,
	git_odb_read_session_cb cb,
	void *payload,
	int depth)
{
	const char *data = git_odb_object_data(tree_obj);
	size_t size = git_odb_object_size(tree_obj);
	const char *p = data;
	const char *end = data + size;
	git_oid_t oid_type = session->odb->options.oid_type;
	int error = 0;

	GIT_UNUSED(tree_oid);

	while (p < end) {
		git_oid entry_oid;
		git_object_t entry_type;
		unsigned int edge;

		if (parse_tree_entry(&p, end, &entry_oid, &entry_type, oid_type) < 0) {
			git_error_set(GIT_ERROR_ODB, "failed to parse tree entry");
			return GIT_EINVALID;
		}

		switch (entry_type) {
		case GIT_OBJECT_TREE:
			edge = GIT_ODB_TRAVERSE_TREE_TREES;
			break;
		case GIT_OBJECT_BLOB:
			edge = GIT_ODB_TRAVERSE_TREE_BLOBS;
			break;
		default:
			/* gitlink: a submodule commit reference, not followed */
			edge = 0;
			break;
		}

		if (edge != 0 && (session->traverse & edge)) {
			if ((error = traverse_object(session, &entry_oid, tree_oid,
					cb, payload, depth + 1)) < 0)
				return error;
		}
	}

	return 0;
}

static int traverse_commit(
	git_odb_read_session *session,
	git_odb_object *commit_obj,
	const git_oid *commit_oid,
	git_odb_read_session_cb cb,
	void *payload,
	int depth)
{
	git_oid tree_oid;
	int error;

	if (!(session->traverse & GIT_ODB_TRAVERSE_COMMIT_TREE))
		return 0;

	if ((error = parse_commit_tree(&tree_oid,
			git_odb_object_data(commit_obj),
			git_odb_object_size(commit_obj),
			session->odb->options.oid_type)) < 0)
		return error;

	return traverse_object(session, &tree_oid, commit_oid,
		cb, payload, depth + 1);
}

static int traverse_tag(
	git_odb_read_session *session,
	git_odb_object *tag_obj,
	const git_oid *tag_oid,
	git_odb_read_session_cb cb,
	void *payload,
	int depth)
{
	git_oid target_oid;
	int error;

	if (!(session->traverse & GIT_ODB_TRAVERSE_TAG_TARGET))
		return 0;

	if ((error = parse_tag_target(&target_oid,
			git_odb_object_data(tag_obj),
			git_odb_object_size(tag_obj),
			session->odb->options.oid_type)) < 0)
		return error;

	return traverse_object(session, &target_oid, tag_oid,
		cb, payload, depth + 1);
}

static int traverse_object(
	git_odb_read_session *session,
	const git_oid *id,
	const git_oid *source,
	git_odb_read_session_cb cb,
	void *payload,
	int depth)
{
	git_odb_object *obj = NULL;
	unsigned int flags = 0;
	bool is_duplicate = false;
	int error;

	if (depth > GIT_ODB_READ_SESSION_MAX_DEPTH) {
		git_error_set(GIT_ERROR_ODB,
			"read session traversal depth exceeded");
		return GIT_EINVALID;
	}

	if ((session->flags & GIT_ODB_READ_SESSION_DEDUP_REFERENCES) &&
	    git_odb_read_session_seen_contains(&session->seen, id)) {
		is_duplicate = true;
	}

	if (is_duplicate) {
		/*
		 * The object was already delivered (and billed).  Read it
		 * again to report the duplicate source; this hits the ODB
		 * cache and is not billed again.
		 */
		if ((error = git_odb_read(&obj, session->odb, id)) < 0)
			return error;
		flags = GIT_ODB_READ_SESSION_OBJ_DUPLICATE;
	} else {
		if ((error = session_deliver(&obj, session, id)) < 0)
			return error;
		flags = GIT_ODB_READ_SESSION_OBJ_DELIVERED;

		if (session->flags & GIT_ODB_READ_SESSION_DEDUP_REFERENCES) {
			git_oid *stable = git__malloc(sizeof(git_oid));
			if (stable == NULL) {
				git_odb_object_free(obj);
				return -1;
			}
			git_oid_cpy(stable, id);
			if (git_odb_read_session_seen_put(&session->seen, stable, true) < 0) {
				git__free(stable);
				git_odb_object_free(obj);
				return -1;
			}
			if (git_vector_insert(&session->seen_oids, stable) < 0) {
				git_odb_read_session_seen_remove(&session->seen, stable);
				git__free(stable);
				git_odb_object_free(obj);
				return -1;
			}
		}
	}

	if ((error = cb(obj, source, flags, payload)) != 0) {
		git_odb_object_free(obj);
		return error;
	}

	/* References are traversed only on the first delivery. */
	if (!is_duplicate) {
		switch (git_odb_object_type(obj)) {
		case GIT_OBJECT_TREE:
			error = traverse_tree(session, obj, id, cb, payload, depth);
			break;
		case GIT_OBJECT_COMMIT:
			error = traverse_commit(session, obj, id, cb, payload, depth);
			break;
		case GIT_OBJECT_TAG:
			error = traverse_tag(session, obj, id, cb, payload, depth);
			break;
		default:
			error = 0;
			break;
		}
	}

	git_odb_object_free(obj);
	return error;
}

int git_odb_read_session_traverse(
	git_odb_read_session *session,
	const git_oid *id,
	git_odb_read_session_cb cb,
	void *payload)
{
	GIT_ASSERT_ARG(session);
	GIT_ASSERT_ARG(id);
	GIT_ASSERT_ARG(cb);

	return traverse_object(session, id, NULL, cb, payload, 0);
}
