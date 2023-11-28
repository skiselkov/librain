/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2023 Saso Kiselkov. All rights reserved.
 */

#if	defined(LIBRAIN_SOIL_VERSION) && LIBRAIN_SOIL_VERSION == 2
#include <SOIL2/SOIL2.h>
#else
#include <SOIL/SOIL.h>
#endif
#include <acfutils/avl.h>
#include <acfutils/glutils.h>
#include <acfutils/glew.h>
#include <acfutils/png.h>
#include <acfutils/safe_alloc.h>
#include <acfutils/thread.h>

#include "objmgr.h"

typedef struct {
	objmgr_t	*mgr;
	GLuint		tex;
	char		*filename;
	unsigned	refcnt;		// protected by objmgr->lock
	int		width;
	int		height;
	int		color_type;
	int		bit_depth;

	mutex_t		lock;
	bool		load_started;
	bool		load_complete;
	bool		allow_dds;
	bool		load_dds;
	bool		load_error;
	uint8_t		*pixels;
	size_t		buflen;		/* used for DDS loads */
	thread_t	loader;

	avl_node_t	node;
} objmgr_tex_t;

struct objmgr_obj_s {
	const char	*filename;	/* held by the obj8_t below */
	obj8_t		*obj;
	unsigned	refcnt;		// protected by objmgr->lock
	bool		load_norm;
	bool		allow_dds_albedo;
	bool		allow_dds_lit;
	bool		drset_needs_update;
	bool		drset_has_changed;
	objmgr_tex_t	*tex;
	objmgr_tex_t	*norm;
	objmgr_tex_t	*lit;
	avl_node_t	node;
};

struct objmgr_s {
	avl_tree_t	texs;		// protected by lock
	avl_tree_t	objs;		// protected by lock
	mutex_t		lock;
};

static int
tex_compar(const void *a, const void *b)
{
	const objmgr_tex_t *ta = a, *tb = b;
	int res = strcmp(ta->filename, tb->filename);

	if (res < 0)
		return (-1);
	if (res > 0)
		return (1);
	return (0);
}

static int
obj_compar(const void *a, const void *b)
{
	const objmgr_obj_t *oa = a, *ob = b;
	int res = strcmp(oa->filename, ob->filename);

	if (res < 0)
		return (-1);
	if (res > 0)
		return (1);
	return (0);
}

static void
free_tex(objmgr_tex_t *tex)
{
	ASSERT(tex != NULL);
	ASSERT0(tex->refcnt);

	if (tex->load_started)
		thread_join(&tex->loader);
	lacf_free(tex->pixels);
	if (tex->tex != 0)
		glDeleteTextures(1, &tex->tex);
	free(tex->filename);
	mutex_destroy(&tex->lock);
	ZERO_FREE(tex);
}

static void
load_texture(void *arg)
{
	objmgr_tex_t *tex;

	ASSERT(arg != NULL);
	tex = arg;
	ASSERT(tex->load_started);
	ASSERT0(tex->load_complete);

	if (tex->allow_dds) {
		char *dds_filename = path_ext_subst(tex->filename, "dds");
		char *dds_filename_up = path_ext_subst(tex->filename, "DDS");
		if ((file_exists(dds_filename, NULL) &&
		    (tex->pixels = file2buf(dds_filename, &tex->buflen)) !=
		    NULL) ||
		    (file_exists(dds_filename_up, NULL) &&
		    (tex->pixels = file2buf(dds_filename_up, &tex->buflen)) !=
		    NULL)) {
			mutex_enter(&tex->lock);
			tex->load_complete = true;
			tex->load_dds = true;
			tex->load_started = false;
			mutex_exit(&tex->lock);
			LACF_DESTROY(dds_filename);
			LACF_DESTROY(dds_filename_up);
			return;
		}
		LACF_DESTROY(dds_filename);
		LACF_DESTROY(dds_filename_up);
	}
	tex->pixels = png_load_from_file_rgb_auto(tex->filename, &tex->width,
	    &tex->height, &tex->color_type, &tex->bit_depth);
	mutex_enter(&tex->lock);
	if (tex->pixels != NULL)
		tex->load_complete = true;
	else
		tex->load_error = true;
	tex->load_started = false;
	mutex_exit(&tex->lock);
}

static objmgr_tex_t *
add_tex(objmgr_t *mgr, const char *filename, bool allow_dds)
{
	const objmgr_tex_t srch = { .filename = (char *)filename };
	avl_index_t where;
	objmgr_tex_t *tex;

	ASSERT(mgr != NULL);
	ASSERT(filename != NULL);

	mutex_enter(&mgr->lock);
	tex = avl_find(&mgr->texs, &srch, &where);
	if (tex == NULL) {
		tex = safe_calloc(1, sizeof (*tex));
		tex->mgr = mgr;
		tex->refcnt = 1;
		tex->filename = safe_strdup(filename);
		tex->allow_dds = allow_dds;
		mutex_init(&tex->lock);

		tex->load_started = true;
		VERIFY(thread_create(&tex->loader, load_texture, tex));

		avl_insert(&mgr->texs, tex, where);
	} else {
		tex->refcnt++;
	}
	mutex_exit(&mgr->lock);

	return (tex);
}

static bool
complete_texture_load(objmgr_tex_t *tex)
{
	ASSERT(tex != NULL);

	mutex_enter(&tex->lock);
	if (tex->load_complete && tex->tex == 0) {
		GLint int_fmt, fmt, type;

		mutex_exit(&tex->lock);

		thread_join(&tex->loader);

		mutex_enter(&tex->lock);
		if (tex->tex != 0) {
			ASSERT3P(tex->pixels, ==, NULL);
			mutex_exit(&tex->lock);
			return (tex->tex != 0);
		}
		ASSERT0(tex->load_started);
		ASSERT(tex->pixels != NULL);

		if (tex->load_dds) {
			tex->tex = SOIL_load_OGL_texture_from_memory(
			    tex->pixels, tex->buflen, 0, 0,
			    SOIL_FLAG_DDS_LOAD_DIRECT);
			if (tex->tex == 0) {
				logMsg("%s: DDS texture load error, file "
				    "data is corrupt", tex->filename);
				tex->load_complete = false;
				tex->load_error = true;
			}
		} else if (glutils_png2gltexfmt(tex->color_type,
		    tex->bit_depth, &int_fmt, &fmt, &type)) {
			glGenTextures(1, &tex->tex);
			VERIFY(tex->tex != 0);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, tex->tex);
			glTexImage2D(GL_TEXTURE_2D, 0, int_fmt, tex->width,
			    tex->height, 0, fmt, type, tex->pixels);
			glGenerateMipmap(GL_TEXTURE_2D);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
			    GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
			    GL_LINEAR_MIPMAP_LINEAR);
		} else {
			logMsg("%s: unsupported color type/bit depth combo: "
			    "%d/%d", tex->filename, tex->color_type,
			    tex->bit_depth);
			tex->load_complete = false;
			tex->load_error = true;
		}
		lacf_free(tex->pixels);
		tex->pixels = NULL;
		GLUTILS_ASSERT_NO_ERROR();
	}
	mutex_exit(&tex->lock);

	return (tex->tex != 0);
}

static void
remove_tex(objmgr_t *mgr, objmgr_tex_t *tex)
{
	ASSERT(mgr != NULL);
	ASSERT(tex != NULL);
	ASSERT3U(tex->refcnt, >, 0);

	mutex_enter(&mgr->lock);
	tex->refcnt--;
	if (tex->refcnt == 0) {
		avl_remove(&mgr->texs, tex);
		free_tex(tex);
	}
	mutex_exit(&mgr->lock);
}

static bool
obj_load_textures(objmgr_t *mgr, objmgr_obj_t *obj)
{
	const char *tex_filename;

	ASSERT(mgr != NULL);
	ASSERT(obj != NULL);
	ASSERT(obj->obj != NULL);

	tex_filename = obj8_get_tex_filename(obj->obj, false);
	if (tex_filename != NULL && obj->tex == NULL) {
		obj->tex = add_tex(mgr, tex_filename, obj->allow_dds_albedo);
		if (obj->tex == NULL)
			return (false);
	}
	if (obj->load_norm) {
		tex_filename = obj8_get_norm_filename(obj->obj, false);
		if (tex_filename != NULL && obj->norm == NULL) {
			obj->norm = add_tex(mgr, tex_filename, false);
			if (obj->norm == NULL)
				return (false);
		}
	}
	tex_filename = obj8_get_lit_filename(obj->obj, false);
	if (tex_filename != NULL && obj->lit == NULL) {
		obj->lit = add_tex(mgr, tex_filename, obj->allow_dds_lit);
		if (obj->lit == NULL)
			return (false);
	}

	return (true);
}

objmgr_t *
objmgr_new(void)
{
	objmgr_t *mgr = safe_calloc(1, sizeof (*mgr));

	avl_create(&mgr->texs, tex_compar, sizeof (objmgr_tex_t),
	    offsetof(objmgr_tex_t, node));
	avl_create(&mgr->objs, obj_compar, sizeof (objmgr_obj_t),
	    offsetof(objmgr_obj_t, node));
	mutex_init(&mgr->lock);

	return (mgr);
}

void
objmgr_destroy(objmgr_t *mgr)
{
	void *cookie;
	objmgr_tex_t *tex;
	objmgr_obj_t *obj;

	if (mgr == NULL)
		return;

	cookie = NULL;
	while ((obj = avl_destroy_nodes(&mgr->objs, &cookie)) != NULL) {
		obj8_free(obj->obj);
		free(obj);
	}
	avl_destroy(&mgr->objs);

	cookie = NULL;
	while ((tex = avl_destroy_nodes(&mgr->texs, &cookie)) != NULL) {
		tex->refcnt = 0;
		free_tex(tex);
	}
	avl_destroy(&mgr->texs);
	mutex_destroy(&mgr->lock);

	free(mgr);
}

objmgr_obj_t *
objmgr_add_obj(objmgr_t *mgr, const char *filename, bool lazy_load_textures,
    bool load_norm, bool allow_dds_albedo, bool allow_dds_lit)
{
	const objmgr_obj_t srch = { .filename = filename };
	objmgr_obj_t *obj;
	avl_index_t where;

	ASSERT(mgr != NULL);
	ASSERT(filename != NULL);

	mutex_enter(&mgr->lock);

	obj = avl_find(&mgr->objs, &srch, &where);
	if (obj == NULL) {
		obj = safe_calloc(1, sizeof (*obj));
		obj->obj = obj8_parse(filename, ZERO_VECT3);
		obj->refcnt = 1;
		obj->load_norm = load_norm;
		obj->allow_dds_albedo = allow_dds_albedo;
		obj->allow_dds_lit = allow_dds_lit;
		if (obj->obj == NULL)
			goto errout;
		obj->filename = obj8_get_filename(obj->obj);
		if (!lazy_load_textures && !obj_load_textures(mgr, obj))
			goto errout;

		avl_insert(&mgr->objs, obj, where);
	} else {
		obj->refcnt++;
	}
	mutex_exit(&mgr->lock);

	return (obj);
errout:
	if (obj->obj != NULL)
		obj8_free(obj->obj);
	if (obj->tex != NULL)
		remove_tex(mgr, obj->tex);
	if (obj->norm != NULL)
		remove_tex(mgr, obj->norm);
	if (obj->lit != NULL)
		remove_tex(mgr, obj->lit);
	ZERO_FREE(obj);
	mutex_exit(&mgr->lock);

	return (NULL);
}

void
objmgr_remove_obj(objmgr_t *mgr, objmgr_obj_t *obj)
{
	ASSERT(mgr != NULL);
	ASSERT(obj != NULL);
	ASSERT(obj->refcnt != 0);

	mutex_enter(&mgr->lock);

	obj->refcnt--;
	if (obj->refcnt == 0) {
		avl_remove(&mgr->objs, obj);

		if (obj->tex != NULL)
			remove_tex(mgr, obj->tex);
		if (obj->norm != NULL)
			remove_tex(mgr, obj->norm);
		if (obj->lit != NULL)
			remove_tex(mgr, obj->lit);
		ASSERT(obj->obj != NULL);
		obj8_free(obj->obj);
		ZERO_FREE(obj);
	}
	mutex_exit(&mgr->lock);
}

obj8_t *
objmgr_get_obj8(const objmgr_obj_t *obj)
{
	ASSERT(obj != NULL);
	ASSERT(obj->obj != NULL);
	return (obj->obj);
}

const char *
objmgr_get_obj_filename(const objmgr_obj_t *obj)
{
	ASSERT(obj != NULL);
	ASSERT(obj->filename != NULL);
	return (obj->filename);
}

static bool
can_complete_tex_load(const objmgr_tex_t *tex)
{
	ASSERT(tex != NULL);
	mutex_enter((mutex_t *)&tex->lock);
	bool result = (tex->load_complete && tex->pixels != NULL);
	mutex_exit((mutex_t *)&tex->lock);
	return (result);
}

bool
objmgr_tex_needs_upload(const objmgr_obj_t *obj)
{
	ASSERT(obj != NULL);
	return ((obj->tex != NULL && can_complete_tex_load(obj->tex)) &&
	    (obj->norm != NULL && can_complete_tex_load(obj->norm)) &&
	    (obj->lit != NULL && can_complete_tex_load(obj->lit)));
}

static unsigned
bind_texture(objmgr_tex_t *tex, unsigned idx, int *out_idx)
{
	/* tex can be NULL */
	ASSERT3U(idx, <, GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS);
	if (out_idx != NULL) {
		if (tex != NULL && complete_texture_load(tex)) {
			ASSERT(tex->tex != 0);
			glActiveTexture(GL_TEXTURE0 + idx);
			glBindTexture(GL_TEXTURE_2D, tex->tex);
			*out_idx = idx;
			idx++;
		} else {
			*out_idx = -1;
		}
	}
	return (idx);
}

void
objmgr_bind_textures(objmgr_t *mgr, objmgr_obj_t *obj, unsigned start_idx,
    int *tex_idx, int *norm_idx, int *lit_idx)
{
	ASSERT(mgr != NULL);
	ASSERT(obj != NULL);

	obj_load_textures(mgr, obj);

	start_idx = bind_texture(obj->tex, start_idx, tex_idx);
	start_idx = bind_texture(obj->norm, start_idx, norm_idx);
	start_idx = bind_texture(obj->lit, start_idx, lit_idx);
}

void
objmgr_foreach_obj(objmgr_t *mgr, objmgr_foreach_cb_t cb, void *userinfo)
{
	ASSERT(mgr != NULL);
	ASSERT(cb != NULL);

	for (objmgr_obj_t *obj = avl_first(&mgr->objs); obj != NULL;
	    obj = AVL_NEXT(&mgr->objs, obj)) {
		cb(mgr, obj, userinfo);
	}
}

void
objmgr_mark_drset_needs_update(objmgr_obj_t *obj)
{
	ASSERT(obj != NULL);
	obj->drset_needs_update = true;
}

bool
objmgr_get_drset_needs_update(const objmgr_obj_t *obj)
{
	ASSERT(obj != NULL);
	return (obj->drset_needs_update);
}

void
objmgr_drset_update(objmgr_obj_t *obj, bool force)
{
	ASSERT(obj != NULL);
	if (obj->drset_needs_update || force) {
		obj->drset_needs_update = false;
		obj->drset_has_changed = obj8_drset_update(
		    obj8_get_drset(obj->obj));
	}
}

bool
objmgr_get_drset_has_changed(const objmgr_obj_t *obj)
{
	ASSERT(obj != NULL);
	return (obj->drset_has_changed);
}

void
objmgr_reset_drset_has_changed(objmgr_obj_t *obj)
{
	ASSERT(obj != NULL);
	obj->drset_has_changed = false;
}

unsigned
objmgr_get_num_objs(const objmgr_t *mgr)
{
	ASSERT(mgr != NULL);
	return (avl_numnodes(&mgr->objs));
}

unsigned
objmgr_get_num_texs(const objmgr_t *mgr)
{
	ASSERT(mgr != NULL);
	return (avl_numnodes(&mgr->texs));
}
