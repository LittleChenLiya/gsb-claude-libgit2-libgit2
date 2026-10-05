/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "common.h"

#include "git2/odb.h"
#include "git2/object.h"

#include "hashmap_oid.h"
#include "oid.h"

/*
 * A record of an object that was delivered through a caller-selected
 * reference edge of a read session, used to deduplicate repeated
 * references while still reporting the source of the first edge.
 */
typedef struct {
	git_oid id;            /* owned; used as the hash key */
	git_oid first_source;  /* source of the edge that first delivered `id` */
	bool has_first_source; /* false when `id` was reached as a root */
} git_odb_delivered;

GIT_HASHMAP_OID_SETUP(git_odb_delivered_map, git_odb_delivered *);

struct git_odb_read_session {
	git_odb *db;             /* not owned */
	uint32_t allowed_types;  /* bitmask of (1u << git_object_t); 0 = all */
	uint64_t max_object_bytes; /* 0 = unlimited */
	uint64_t max_total_bytes;  /* 0 = unlimited */
	uint64_t spent;            /* decompressed bytes delivered so far */
	git_odb_delivered_map delivered;
};

int git_odb_read_session_options_init(
	git_odb_read_session_options *opts,
	unsigned int version)
{
	GIT_INIT_STRUCTURE_FROM_TEMPLATE(
		opts, version, git_odb_read_session_options,
		GIT_ODB_READ_SESSION_OPTIONS_INIT);
	return 0;
}

int git_odb_read_session_open(
	git_odb_read_session **out,
	git_odb *db,
	const git_odb_read_session_options *opts)
{
	git_odb_read_session *session;

	GIT_ASSERT_ARG(out);
	GIT_ASSERT_ARG(db);
	GIT_ERROR_CHECK_VERSION(opts, GIT_ODB_READ_SESSION_OPTIONS_VERSION,
		"git_odb_read_session_options");

	session = git__calloc(1, sizeof(git_odb_read_session));
	GIT_ERROR_CHECK_ALLOC(session);

	session->db = db;

	if (opts) {
		session->allowed_types = opts->allowed_types;
		session->max_object_bytes = opts->max_object_bytes;
		session->max_total_bytes = opts->max_total_bytes;
	}

	*out = session;
	return 0;
}

void git_odb_read_session_free(git_odb_read_session *session)
{
	git_hashmap_iter_t iter = 0;
	const git_oid *key;
	git_odb_delivered *rec;

	if (session == NULL)
		return;

	while (git_odb_delivered_map_iterate(
			&iter, &key, &rec, &session->delivered) == 0)
		git__free(rec);

	git_odb_delivered_map_dispose(&session->delivered);
	git__free(session);
}

uint64_t git_odb_read_session_spent(git_odb_read_session *session)
{
	GIT_ASSERT_ARG_WITH_RETVAL(session, 0);
	return session->spent;
}

static bool session_type_allowed(
	git_odb_read_session *session,
	git_object_t type)
{
	if (session->allowed_types == 0)
		return true;
	if (type <= 0 || type >= 32)
		return false;
	return (session->allowed_types & (1u << (unsigned int)type)) != 0;
}

int git_odb_read_session_read(
	git_odb_object **out,
	git_odb_read_session *session,
	const git_oid *id)
{
	git_odb_object *obj = NULL;
	git_object_t type;
	size_t len;
	int error;

	GIT_ASSERT_ARG(out);
	GIT_ASSERT_ARG(session);
	GIT_ASSERT_ARG(id);

	*out = NULL;

	/*
	 * Read through the ordinary ODB path; the object may be served
	 * from the ODB cache, but cache hits are still billed as bytes
	 * delivered by this session.
	 */
	if ((error = git_odb_read(&obj, session->db, id)) < 0)
		return error;

	type = git_odb_object_type(obj);
	len = git_odb_object_size(obj);

	/*
	 * The stored type is authoritative: an object whose type is not
	 * allowed is rejected outright, never decoded as another type.
	 */
	if (!session_type_allowed(session, type)) {
		git_error_set(GIT_ERROR_ODB,
			"object type '%s' is not allowed by this read session",
			git_object_type2string(type));
		error = GIT_EINVALID;
		goto out;
	}

	if (session->max_object_bytes != 0 &&
	    (uint64_t)len > session->max_object_bytes) {
		git_error_set(GIT_ERROR_ODB,
			"object exceeds the per-object byte budget of this read session");
		error = GIT_EBUDGET;
		goto out;
	}

	if (session->max_total_bytes != 0 &&
	    (uint64_t)len > session->max_total_bytes - session->spent) {
		git_error_set(GIT_ERROR_ODB,
			"the total byte budget of this read session is exhausted");
		error = GIT_EBUDGET;
		goto out;
	}

	session->spent += (uint64_t)len;
	*out = obj;
	obj = NULL;
	error = 0;

out:
	/* errors deliver nothing and bill nothing */
	git_odb_object_free(obj);
	return error;
}

int git_odb_read_session_follow(
	git_odb_object **out,
	git_oid *first_source,
	git_odb_read_session *session,
	const git_oid *from,
	const git_oid *to)
{
	git_odb_delivered *rec, *new_rec;
	git_odb_object *obj = NULL;
	int error;

	GIT_ASSERT_ARG(session);
	GIT_ASSERT_ARG(to);

	if (out)
		*out = NULL;
	if (first_source)
		git_oid_clear(first_source, git_oid_type(to));

	/*
	 * Repeated reference: the object was already delivered through
	 * another edge of this traversal.  Deduplicate it -- do not
	 * deliver or bill it again -- but report the source of the edge
	 * that first delivered it.
	 */
	if (git_odb_delivered_map_get(&rec, &session->delivered, to) == 0) {
		if (first_source && rec->has_first_source)
			git_oid_cpy(first_source, &rec->first_source);
		return GIT_ITEROVER;
	}

	if ((error = git_odb_read_session_read(&obj, session, to)) < 0)
		return error;

	new_rec = git__malloc(sizeof(git_odb_delivered));
	if (new_rec == NULL) {
		git_odb_object_free(obj);
		GIT_ERROR_CHECK_ALLOC(NULL);
	}

	git_oid_cpy(&new_rec->id, to);
	new_rec->has_first_source = (from != NULL);
	if (from)
		git_oid_cpy(&new_rec->first_source, from);

	if ((error = git_odb_delivered_map_put(
			&session->delivered, &new_rec->id, new_rec)) < 0) {
		git__free(new_rec);
		git_odb_object_free(obj);
		return error;
	}

	if (out)
		*out = obj;
	else
		git_odb_object_free(obj);

	return 0;
}
