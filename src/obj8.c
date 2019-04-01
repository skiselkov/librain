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
 * Copyright 2019 Saso Kiselkov. All rights reserved.
 */

#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <acfutils/assert.h>
#include <acfutils/helpers.h>
#include <acfutils/glutils.h>
#include <acfutils/log.h>
#include <acfutils/math.h>
#include <acfutils/perf.h>
#include <acfutils/safe_alloc.h>
#include <acfutils/thread.h>

#include "glpriv.h"
#include "librain.h"
#include "obj8.h"

#define	ANIM_ALLOC_STEP	8
/*
 * After rendering starts, we only try to lookup datarefs for a little
 * while. If within 10 draw cycles the dataref isn't populated, we give
 * up to avoid lags performing costly dataref lookups for datarefs that
 * are likely never going to show up.
 */
#define	MAX_DR_LOOKUPS	10

#define	PROT_BAD_ANIM_VAL(__val, __cmd, __bail) \
	do { \
		if (!isfinite((__val))) { \
			logMsg("Bad animation dataref %s = %f. Bailing out. " \
			    "Report this as a bug and attach the log file.", \
			    (__cmd)->dr_name, (__val)); \
			__bail; \
		} \
	} while (0)

TEXSZ_MK_TOKEN(obj8_vtx_buf);
TEXSZ_MK_TOKEN(obj8_idx_buf);

typedef enum {
	OBJ8_CMD_GROUP,
	OBJ8_CMD_TRIS,
	OBJ8_CMD_ANIM_HIDE_SHOW,
	OBJ8_CMD_ANIM_TRANS,
	OBJ8_CMD_ANIM_ROTATE,
	OBJ8_NUM_CMDS
} obj8_cmd_type_t;

typedef struct {
	unsigned	vtx_off;	/* offset into index table */
	unsigned	n_vtx;		/* number of vertices in geometry */
	char		group_id[32];	/* Contents of X-GROUP-ID attribute */
	bool_t		double_sided;
	list_node_t	node;
} obj8_geom_t;

typedef struct obj8_cmd_s {
	obj8_cmd_type_t		type;
	struct obj8_cmd_s	*parent;
	dr_t			dr;
	int			dr_offset;
	char			dr_name[128];
	bool_t			dr_found;
	unsigned		dr_lookup_done;
	union {
		struct {
			list_t	cmds;
		} group;
		struct {
			double	val[2];
			bool_t	set_val;
		} hide_show;
		struct {
			size_t	n_pts;
			size_t	n_pts_cap;
			vect2_t	*pts;
			vect3_t	axis;
		} rotate;
		struct {
			size_t	n_pts;
			size_t	n_pts_cap;
			double	*values;
			vect3_t	*pos;
		} trans;
		obj8_geom_t	tris;
	};
	list_node_t	list_node;
} obj8_cmd_t;

struct obj8_s {
	char		*filename;

	GLuint		last_prog;
	GLint		pvm_loc;
	GLint		pos_loc;
	GLint		norm_loc;
	GLint		tex0_loc;
	void		*vtx_table;
	GLuint		vtx_buf;
	unsigned	vtx_cap;
	void		*idx_table;
	GLuint		idx_buf;
	unsigned	idx_cap;
	mat4		matrix;
	obj8_cmd_t	*top;

	thread_t	loader;
	mutex_t		lock;
	condvar_t	cv;
	bool_t		load_complete;
	bool_t		load_error;
	bool_t		load_stop;
};

typedef struct {
	FILE		*fp;
	vect3_t		pos_offset;
	vect3_t		cg_offset;
	obj8_t		*obj;
} obj8_load_info_t;

typedef struct {
	GLfloat		x;
	GLfloat		y;
} vec2_32f_t;

typedef struct {
	GLfloat		x;
	GLfloat		y;
	GLfloat		z;
} vec3_32f_t;

typedef struct {
	vec3_32f_t	pos;
	vec3_32f_t	norm;
	vec2_32f_t	tex;
} obj8_vtx_t;

static void
obj8_geom_init(obj8_geom_t *geom, const char *group_id, bool_t double_sided,
    unsigned off, unsigned len, GLuint vtx_cap, const GLuint *idx_table,
    GLuint idx_cap)
{
	geom->vtx_off = off;
	geom->n_vtx = len;
	strlcpy(geom->group_id, group_id, sizeof (geom->group_id));
	geom->double_sided = double_sided;

	for (GLuint x = geom->vtx_off; x < geom->vtx_off + geom->n_vtx; x++) {
		GLuint idx;

		ASSERT3U(x, <, idx_cap);
		idx = idx_table[x];
		ASSERT3U(idx, <, vtx_cap);
	}
}

static obj8_cmd_t *
obj8_cmd_alloc(obj8_cmd_type_t type, obj8_cmd_t *parent)
{
	obj8_cmd_t *cmd = safe_calloc(1, sizeof (*cmd));

	cmd->type = type;
	cmd->parent = parent;
	if (parent != NULL) {
		ASSERT3U(parent->type, ==, OBJ8_CMD_GROUP);
		list_insert_tail(&parent->group.cmds, cmd);
	} else {
		ASSERT3U(type, ==, OBJ8_CMD_GROUP);
	}
	if (type == OBJ8_CMD_GROUP) {
		list_create(&cmd->group.cmds, sizeof (obj8_cmd_t),
		    offsetof(obj8_cmd_t, list_node));
	}

	return (cmd);
}

static bool_t
parse_hide_show(bool_t set_val, const char *fmt, const char *line,
    const char *filename, int linenr, obj8_cmd_t *parent)
{
	char dr_name[256] = { 0 };
	obj8_cmd_t *cmd = obj8_cmd_alloc(OBJ8_CMD_ANIM_HIDE_SHOW, parent);
	int n = sscanf(line, fmt, &cmd->hide_show.val[0],
	    &cmd->hide_show.val[1], dr_name);

	if (n != 2 && n != 3) {
		logMsg("%s:%d: parsing of ANIM_{hide,show} line failed",
		    filename, linenr);
		return (B_FALSE);
	}
	cmd->hide_show.set_val = set_val;
	strlcpy(cmd->dr_name, dr_name, sizeof (cmd->dr_name));

	return (B_TRUE);
}

static bool_t
anim_group_check(obj8_cmd_t *cmd, obj8_cmd_type_t type, const char *name,
    const char *filename, int linenr)
{
	if (cmd == NULL) {
		logMsg("%s:%d: failed to parse ANIM_%s_key, NOT inside "
		    "existing animation group", filename, linenr, name);
		return (B_FALSE);
	}
	if (cmd->type != type) {
		logMsg("%s:%d: failed to parse ANIM_%s_key, NOT inside "
		    "ANIM_%s_begin anim group", filename, linenr, name, name);
		return (B_FALSE);
	}

	return (B_TRUE);
}

static bool_t
parse_trans_key(const char *line, obj8_cmd_t *cmd, const char *filename,
    int linenr)
{
	size_t n;

	if (!anim_group_check(cmd, OBJ8_CMD_ANIM_TRANS, "trans",
	    filename, linenr))
		return (B_FALSE);

	if (cmd->trans.n_pts_cap == cmd->trans.n_pts) {
		int new_cap = cmd->trans.n_pts_cap + ANIM_ALLOC_STEP;

		cmd->trans.values = realloc(cmd->trans.values,
		    new_cap * sizeof (*cmd->trans.values));
		cmd->trans.pos = realloc(cmd->trans.pos,
		    new_cap * sizeof (*cmd->trans.pos));
		cmd->trans.n_pts_cap = new_cap;
	}
	n = cmd->trans.n_pts;
	cmd->trans.n_pts++;
	if (sscanf(line, "ANIM_trans_key %lf %lf %lf %lf",
	    &cmd->trans.values[n], &cmd->trans.pos[n].x,
	    &cmd->trans.pos[n].y, &cmd->trans.pos[n].z) != 4) {
		logMsg("%s:%d: failed to parse ANIM_trans_key",
		    filename, linenr);
		return (B_FALSE);
	}

	return (B_TRUE);
}

/*
 * X-Plane wraps rotations around the "shorter way", so here we adjust
 * rotation offsets in terms of deltas from the previous point.
 */
static void
normalize_rotate_angle(obj8_cmd_t *cmd, size_t n)
{
	double prev, delta;

	ASSERT3U(cmd->type, ==, OBJ8_CMD_ANIM_ROTATE);
	ASSERT3U(cmd->rotate.n_pts, >, n);

	if (n == 0)
		prev = 0;
	else
		prev = cmd->rotate.pts[n - 1].y;
	delta = cmd->rotate.pts[n].y - prev;
	while (delta > 180)
		delta -= 360;
	while (delta < -180)
		delta += 360;
	cmd->rotate.pts[n].y = prev + delta;
}

static bool_t
parse_rotate_key(const char *line, obj8_cmd_t *cmd, const char *filename,
    int linenr)
{
	size_t n;

	if (!anim_group_check(cmd, OBJ8_CMD_ANIM_ROTATE, "rotate",
	    filename, linenr))
		return (B_FALSE);

	if (cmd->rotate.n_pts_cap == cmd->rotate.n_pts) {
		int new_cap = cmd->rotate.n_pts_cap + ANIM_ALLOC_STEP;

		cmd->rotate.pts = realloc(cmd->rotate.pts,
		    new_cap * sizeof (*cmd->rotate.pts));
		cmd->rotate.n_pts_cap = new_cap;
	}
	n = cmd->rotate.n_pts;
	cmd->rotate.n_pts++;
	if (sscanf(line, "ANIM_rotate_key %lf %lf", &cmd->rotate.pts[n].x,
	    &cmd->rotate.pts[n].y) != 2) {
		logMsg("%s:%d: failed to parse ANIM_rotate_key",
		    filename, linenr);
		return (B_FALSE);
	}
	normalize_rotate_angle(cmd, n);

	return (B_TRUE);
}

static void
obj8_parse_worker(void *userinfo)
{
	obj8_t		*obj;
	obj8_vtx_t	*vtx_table = NULL;
	GLuint		*idx_table = NULL;
	GLuint		vtx_cap = 0;
	GLuint		idx_cap = 0;
	char		*line = NULL;
	size_t		cap = 0;
	unsigned	cur_vtx = 0;
	unsigned	cur_idx = 0;
	char		group_id[32] = { 0 };
	bool_t		double_sided = B_FALSE;
	obj8_cmd_t	*cur_cmd = NULL;
	obj8_cmd_t	*cur_anim = NULL;
	vect3_t		offset;

	obj8_load_info_t *info;
	const char	*filename;
	FILE		*fp;
	vect3_t		pos_offset;

	ASSERT(userinfo != NULL);
	info = userinfo;
	fp = info->fp;
	obj = info->obj;
	filename = obj->filename;
	pos_offset = info->pos_offset;

	obj->top = cur_cmd = obj8_cmd_alloc(OBJ8_CMD_GROUP, NULL);

	offset = vect3_add(pos_offset, info->cg_offset);
	glm_translate_make(obj->matrix, (vec3){offset.x, offset.y, offset.z});

	for (int linenr = 1; getline(&line, &cap, fp) > 0 && !obj->load_stop;
	    linenr++) {
		strip_space(line);

		if (strncmp(line, "VT", 2) == 0) {
			obj8_vtx_t *vtx;
			if (cur_vtx >= vtx_cap) {
				logMsg("%s:%d: too many VT lines found",
				    filename, linenr);
				goto errout;
			}
			vtx = &vtx_table[cur_vtx];
			if (sscanf(line, "VT %f %f %f %f %f %f %f %f",
			    &vtx->pos.x, &vtx->pos.y, &vtx->pos.z,
			    &vtx->norm.x, &vtx->norm.y, &vtx->norm.z,
			    &vtx->tex.x, &vtx->tex.y) != 8) {
				logMsg("%s:%d: parsing of VT line failed",
				    filename, linenr);
				goto errout;
			}
			cur_vtx++;
		} else if (strncmp(line, "IDX10", 5) == 0) {
			if (cur_idx + 10 > idx_cap) {
				logMsg("%s:%d: too many IDX10 lines found",
				    filename, linenr);
				goto errout;
			}
			if (sscanf(line, "IDX10 %u %u %u %u %u %u %u %u %u %u",
			    &idx_table[cur_idx + 0], &idx_table[cur_idx + 1],
			    &idx_table[cur_idx + 2], &idx_table[cur_idx + 3],
			    &idx_table[cur_idx + 4], &idx_table[cur_idx + 5],
			    &idx_table[cur_idx + 6], &idx_table[cur_idx + 7],
			    &idx_table[cur_idx + 8], &idx_table[cur_idx + 9]) !=
			    10) {
				logMsg("%s:%d: parsing of IDX10 line failed",
				    filename, linenr);
				goto errout;
			}
			for (int i = 0; i < 10; i++) {
				if (idx_table[cur_idx + i] >= vtx_cap) {
					logMsg("%s:%d: index entry %d falls "
					    "outside of vertex table",
					    filename, linenr, i);
					goto errout;
				}
			}
			cur_idx += 10;
		} else if (strncmp(line, "IDX", 3) == 0) {
			if (cur_idx >= idx_cap) {
				logMsg("%s:%d: too many IDX lines found",
				    filename, linenr);
				goto errout;
			}
			if (sscanf(line, "IDX %u", &idx_table[cur_idx]) != 1) {
				logMsg("%s:%d: parsing of IDX line failed",
				    filename, linenr);
				goto errout;
			}
			if (idx_table[cur_idx] >= vtx_cap) {
				logMsg("%s:%d: index entry falls outside of "
				    "vertex table", filename, linenr);
			}
			cur_idx++;
		} else if (strncmp(line, "TRIS", 4) == 0) {
			obj8_cmd_t *cmd;
			unsigned off, len;

			if (sscanf(line, "TRIS %u %u", &off, &len) != 2) {
				logMsg("%s:%d: parsing of TRIS line failed",
				    filename, linenr);
				goto errout;
			}
			if (off + len > idx_cap) {
				logMsg("%s:%d: index table offsets are invalid",
				    filename, linenr);
				goto errout;
			}
			cmd = obj8_cmd_alloc(OBJ8_CMD_TRIS, cur_cmd);
			obj8_geom_init(&cmd->tris, group_id, double_sided,
			    off, len, vtx_cap, idx_table, idx_cap);
		} else if (strncmp(line, "ANIM_begin", 10) == 0) {
			cur_cmd = obj8_cmd_alloc(OBJ8_CMD_GROUP, cur_cmd);
		} else if (strncmp(line, "ANIM_end", 10) == 0) {
			if (cur_cmd->parent == NULL) {
				logMsg("%s:%d: invalid ANIM_end, not inside "
				    "an animation group.", filename, linenr);
				goto errout;
			}
			cur_cmd = cur_cmd->parent;
		} else if (strncmp(line, "ANIM_show", 9) == 0) {
			if (!parse_hide_show(B_TRUE, "ANIM_show %lf %lf %255s",
			    line, filename, linenr, cur_cmd))
				goto errout;
		} else if (strncmp(line, "ANIM_hide", 9) == 0) {
			if (!parse_hide_show(B_FALSE, "ANIM_hide %lf %lf %255s",
			    line, filename, linenr, cur_cmd))
				goto errout;
		} else if (strncmp(line, "ANIM_trans_begin", 16) == 0) {
			char dr_name[256];
			obj8_cmd_t *cmd;

			if (cur_anim != NULL) {
				logMsg("%s:%d: failed to parse "
				    "ANIM_trans_begin, inside existing "
				    "anim group", filename, linenr);
				goto errout;
			}
			if (sscanf(line, "ANIM_trans_begin %255s", dr_name) !=
			    1) {
				logMsg("%s:%d: failed to parse "
				    "ANIM_trans_begin", filename, linenr);
				goto errout;
			}
			cmd = obj8_cmd_alloc(OBJ8_CMD_ANIM_TRANS, cur_cmd);
			strlcpy(cmd->dr_name, dr_name, sizeof (cmd->dr_name));
			cur_anim = cmd;
		} else if (strncmp(line, "ANIM_rotate_begin", 17) == 0) {
			char dr_name[256];
			obj8_cmd_t *cmd;

			if (cur_anim != NULL) {
				logMsg("%s:%d: failed to parse "
				    "ANIM_rotate_begin, inside existing "
				    "anim group", filename, linenr);
				goto errout;
			}
			cmd = obj8_cmd_alloc(OBJ8_CMD_ANIM_ROTATE, cur_cmd);
			if (sscanf(line, "ANIM_rotate_begin %lf %lf %lf %255s",
			    &cmd->rotate.axis.x, &cmd->rotate.axis.y,
			    &cmd->rotate.axis.z, dr_name) != 4) {
				logMsg("%s:%d: failed to parse "
				    "ANIM_rotate_begin", filename, linenr);
				goto errout;
			}
			strlcpy(cmd->dr_name, dr_name, sizeof (cmd->dr_name));
			cur_anim = cmd;
		} else if (strncmp(line, "ANIM_trans_end", 14) == 0 ||
		    strncmp(line, "ANIM_rotate_end", 15) == 0) {
			if (cur_anim == NULL) {
				logMsg("%s:%d: failed to parse "
				    "ANIM_{rotate,trans}_end, NOT inside "
				    "existing anim group", filename, linenr);
				goto errout;
			}
			cur_anim = NULL;
		} else if (strncmp(line, "ANIM_trans_key", 14) == 0) {
			if (!parse_trans_key(line, cur_anim, filename, linenr))
				goto errout;
		} else if (strncmp(line, "ANIM_rotate_key", 15) == 0) {
			if (!parse_rotate_key(line, cur_anim, filename, linenr))
				goto errout;
		} else if (strncmp(line, "ANIM_trans", 10) == 0) {
			char dr_name[256] = { 0 };
			obj8_cmd_t *cmd;
			int l;

			cmd = obj8_cmd_alloc(OBJ8_CMD_ANIM_TRANS, cur_cmd);
			cmd->trans.n_pts = 2;
			cmd->trans.n_pts_cap = 2;
			cmd->trans.values =
			    calloc(sizeof (*cmd->trans.values), 2);
			cmd->trans.pos = calloc(sizeof (*cmd->trans.pos), 2);
			l = sscanf(line, "ANIM_trans %lf %lf %lf %lf %lf %lf "
			    "%lf %lf %255s",
			    &cmd->trans.pos[0].x, &cmd->trans.pos[0].y,
			    &cmd->trans.pos[0].z, &cmd->trans.pos[1].x,
			    &cmd->trans.pos[1].y, &cmd->trans.pos[1].z,
			    &cmd->trans.values[0], &cmd->trans.values[1],
			    dr_name);
			if (l < 6) {
				logMsg("%s:%d: failed to parse ANIM_trans (%d)",
				    filename, linenr, l);
				goto errout;
			}
			strlcpy(cmd->dr_name, dr_name, sizeof (cmd->dr_name));
		} else if (strncmp(line, "ANIM_rotate", 11) == 0) {
			char dr_name[256] = { 0 };
			obj8_cmd_t *cmd;

			cmd = obj8_cmd_alloc(OBJ8_CMD_ANIM_ROTATE, cur_cmd);
			cmd->rotate.n_pts = 2;
			cmd->rotate.n_pts_cap = 2;
			cmd->rotate.pts = calloc(sizeof (*cmd->rotate.pts), 2);
			if (sscanf(line, "ANIM_rotate %lf %lf %lf %lf %lf "
			    "%lf %lf %255s",
			    &cmd->rotate.axis.x, &cmd->rotate.axis.y,
			    &cmd->rotate.axis.z,
			    &cmd->rotate.pts[0].y, &cmd->rotate.pts[1].y,
			    &cmd->rotate.pts[0].x, &cmd->rotate.pts[1].x,
			    dr_name) < 5) {
				logMsg("%s:%d: failed to parse ANIM_rotate",
				    filename, linenr);
				goto errout;
			}
			strlcpy(cmd->dr_name, dr_name, sizeof (cmd->dr_name));
			normalize_rotate_angle(cmd, 1);
		} else if (strncmp(line, "POINT_COUNTS", 12) == 0) {
			unsigned lines, lites;

			if (vtx_table != NULL) {
				logMsg("%s:%d: duplicate POINT_COUNTS line "
				    "found", filename, linenr);
				goto errout;
			}
			if (sscanf(line, "POINT_COUNTS %u %u %u %u",
			    &vtx_cap, &lines, &lites, &idx_cap) != 4) {
				logMsg("%s:%d: parsing of POINT_COUNTS failed",
				    filename, linenr);
				goto errout;
			}
			vtx_table = safe_calloc(vtx_cap, sizeof (*vtx_table));
			idx_table = safe_calloc(idx_cap, sizeof (*idx_table));
		} else if (strncmp(line, "X-GROUP-ID", 10) == 0) {
			if (sscanf(line, "X-GROUP-ID %31s", group_id) != 1)
				*group_id = 0;
		} else if (strncmp(line, "X-DOUBLE-SIDED", 14) == 0) {
			double_sided = B_TRUE;
		} else if (strncmp(line, "X-SINGLE-SIDED", 14) == 0) {
			double_sided = B_FALSE;
		}
	}

	obj->vtx_table = vtx_table;
	obj->idx_table = idx_table;

	obj->vtx_cap = vtx_cap;
	obj->idx_cap = idx_cap;

	free(line);

	fclose(info->fp);
	free(info);

	mutex_enter(&obj->lock);
	obj->load_complete = B_TRUE;
	cv_broadcast(&obj->cv);
	mutex_exit(&obj->lock);

	return;
errout:
	free(vtx_table);
	free(idx_table);
	free(line);

	fclose(info->fp);
	free(info);

	mutex_enter(&obj->lock);
	obj->load_complete = B_TRUE;
	obj->load_error = B_TRUE;
	cv_broadcast(&obj->cv);
	mutex_exit(&obj->lock);
}

static obj8_t *
obj8_parse_fp(FILE *fp, const char *filename, vect3_t pos_offset)
{
	obj8_load_info_t *info;
	dr_t cgY_orig, cgZ_orig;
	obj8_t *obj;

	fdr_find(&cgY_orig, "sim/aircraft/weight/acf_cgY_original");
	fdr_find(&cgZ_orig, "sim/aircraft/weight/acf_cgZ_original");

	obj = safe_calloc(1, sizeof (*obj));
	mutex_init(&obj->lock);
	cv_init(&obj->cv);
	obj->filename = strdup(filename);

	info = safe_calloc(1, sizeof (*info));
	info->fp = fp;
	info->pos_offset = pos_offset;
	info->obj = obj;
	info->cg_offset = VECT3(0, -FEET2MET(dr_getf(&cgY_orig)),
	    -FEET2MET(dr_getf(&cgZ_orig)));

	VERIFY(thread_create(&obj->loader, obj8_parse_worker, info));

	return (obj);
}

static bool_t
upload_data(obj8_t *obj)
{
	if (!obj->load_complete) {
		mutex_enter(&obj->lock);
		while (!obj->load_complete)
			cv_wait(&obj->cv, &obj->lock);
		mutex_exit(&obj->lock);
	}
	/*
	 * Once the initial data load is complete, upload the tables and
	 * dispose of the in-memory copies as they are no longer needed.
	 */
	if (obj->vtx_buf == 0 && !obj->load_error) {
		ASSERT(obj->vtx_table != NULL);
		ASSERT(obj->idx_table != NULL);

		/* Make sure outside GL errors don't confuse us */
		glutils_reset_errors();

		glGenBuffers(1, &obj->vtx_buf);
		glBindBuffer(GL_ARRAY_BUFFER, obj->vtx_buf);
		glBufferData(GL_ARRAY_BUFFER, obj->vtx_cap *
		    sizeof (obj8_vtx_t), obj->vtx_table, GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		IF_TEXSZ(TEXSZ_ALLOC_BYTES_INSTANCE(obj8_vtx_buf, obj,
		    obj->filename, 0, obj->vtx_cap * sizeof (obj8_vtx_t)));
		free(obj->vtx_table);
		obj->vtx_table = NULL;

		glGenBuffers(1, &obj->idx_buf);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, obj->idx_buf);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, obj->idx_cap *
		    sizeof (GLuint), obj->idx_table, GL_STATIC_DRAW);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
		IF_TEXSZ(TEXSZ_ALLOC_BYTES_INSTANCE(obj8_idx_buf, obj,
		    obj->filename, 0, obj->idx_cap * sizeof (GLuint)));
		free(obj->idx_table);
		obj->idx_table = NULL;

		GLUTILS_ASSERT_NO_ERROR();
	}
	return (!obj->load_error);
}

obj8_t *
obj8_parse(const char *filename, vect3_t pos_offset)
{
	FILE *fp;
	obj8_t *obj;

	if (!librain_glob_init())
		return (NULL);
	fp = fopen(filename, "rb");

	if (fp == NULL) {
		logMsg("Can't open %s: %s", filename, strerror(errno));
		return (NULL);
	}
	/* obj8_parse_fp takes ownership of the file handle */
	obj = obj8_parse_fp(fp, filename, pos_offset);

	return (obj);
}

static void
obj8_cmd_free(obj8_cmd_t *cmd)
{
	switch (cmd->type) {
	case OBJ8_CMD_GROUP: {
		obj8_cmd_t *subcmd;

		while ((subcmd = list_remove_head(&cmd->group.cmds)) != NULL)
			obj8_cmd_free(subcmd);
		list_destroy(&cmd->group.cmds);
		break;
	}
	case OBJ8_CMD_TRIS:
	case OBJ8_CMD_ANIM_HIDE_SHOW:
		break;
	case OBJ8_CMD_ANIM_TRANS:
		free(cmd->trans.values);
		free(cmd->trans.pos);
		break;
	case OBJ8_CMD_ANIM_ROTATE:
		free(cmd->rotate.pts);
		break;
	default:
		VERIFY(0);
	}
	free(cmd);
}

void
obj8_free(obj8_t *obj)
{
	obj8_cmd_free(obj->top);

	obj->load_stop = B_TRUE;
	thread_join(&obj->loader);

	if (obj->vtx_buf != 0) {
		glDeleteBuffers(1, &obj->vtx_buf);
		IF_TEXSZ(TEXSZ_FREE_BYTES_INSTANCE(obj8_vtx_buf, obj,
		    obj->vtx_cap * sizeof (obj8_vtx_t)));
	}
	free(obj->vtx_table);
	if (obj->idx_buf != 0) {
		glDeleteBuffers(1, &obj->idx_buf);
		IF_TEXSZ(TEXSZ_FREE_BYTES_INSTANCE(obj8_idx_buf, obj,
		    obj->idx_cap * sizeof (GLuint)));
	}
	free(obj->idx_table);
	free(obj->filename);

	free(obj);
}

static bool_t
find_dr_with_offset(char *dr_name, dr_t *dr, int *offset)
{
	char *bracket;

	if (dr_find(dr, "%s", dr_name)) {
		*offset = -1;
		return (B_TRUE);
	}

	bracket = strrchr(dr_name, '[');
	if (bracket != NULL) {
		int cap;

		*bracket = 0;
		if (!dr_find(dr, "%s", dr_name))
			return (B_FALSE);
		cap = dr_getvi(dr, NULL, 0, 0);
		if (cap == 0)
			return (B_FALSE);
		*offset = clampi(atoi(&bracket[1]), 0, cap - 1);
		return (B_TRUE);
	}

	return (B_FALSE);
}

static double
cmd_dr_read(obj8_cmd_t *cmd)
{
	double v;

	if (COND_UNLIKELY(!cmd->dr_found)) {
		if (COND_LIKELY(cmd->dr_lookup_done > MAX_DR_LOOKUPS))
			return (0);
		cmd->dr_lookup_done++;
		if (!find_dr_with_offset(cmd->dr_name, &cmd->dr,
		    &cmd->dr_offset)) {
			return (0);
		}
		cmd->dr_found = B_TRUE;
	}
	if (cmd->dr_offset > 0)
		dr_getvf(&cmd->dr, &v, cmd->dr_offset, 1);
	else
		v = dr_getf(&cmd->dr);

	return (v);
}

static void
geom_draw(const obj8_t *obj, const obj8_geom_t *geom, const mat4 pvm)
{
	if (obj->pvm_loc != -1)
		glUniformMatrix4fv(obj->pvm_loc, 1, GL_FALSE, (void *)pvm);
	glDrawElements(GL_TRIANGLES, geom->n_vtx, GL_UNSIGNED_INT,
	    (void *)(geom->vtx_off * sizeof (GLuint)));
}

static inline double
anim_extrapolate_1D(double *v_p, vect2_t v1, vect2_t v2)
{
	if ((v1.x < v2.x && (*v_p < v1.x || *v_p > v2.x)) ||
	    (v1.x > v2.x && (*v_p > v1.x || *v_p < v2.x))) {
		*v_p = fx_lin(*v_p, v1.x, v1.y, v2.x, v2.y);
		return (B_TRUE);
	}
	return (B_FALSE);
}

static double
rotation_get_angle(obj8_cmd_t *cmd)
{
	double val = cmd_dr_read(cmd);
	size_t n = cmd->rotate.n_pts;

	PROT_BAD_ANIM_VAL(val, cmd, return (0));
	/* Too few points to animate anything */
	if (n == 0)
		return (0);
	if (n == 1)
		return (cmd->rotate.pts[0].y);
	/* Invalid range, but return first rotation offset */
	if (cmd->rotate.pts[0].x == cmd->rotate.pts[n - 1].x)
		return (cmd->rotate.pts[0].y);

	if (anim_extrapolate_1D(&val, cmd->rotate.pts[0],
	    cmd->rotate.pts[n - 1]))
		return (val);

	for (size_t i = 0; i + 1 < n; i++) {
		double v1 = cmd->rotate.pts[i].x;
		double v2 = cmd->rotate.pts[i + 1].x;
		if (v1 < v2 && v1 <= val && val <= v2) {
			double rat = (val - v1) / (v2 - v1);

			return (wavg(cmd->rotate.pts[i].y,
			    cmd->rotate.pts[i + 1].y, rat));
		} else if (v2 < v1 && v2 <= val && val <= v1) {
			double rat = (val - v2) / (v1 - v2);
			return (wavg(cmd->rotate.pts[i].y,
			    cmd->rotate.pts[i + 1].y, 1 - rat));
		}
	}

	VERIFY_MSG(0, "Something went really wrong during animation "
	    "of %s with value %f", cmd->dr.name, val);
}

void
obj8_draw_group_cmd(const obj8_t *obj, obj8_cmd_t *cmd, const char *groupname,
    const mat4 pvm_in)
{
	bool_t do_show = B_TRUE;
	mat4 pvm;

	UNUSED(obj);
	ASSERT3U(cmd->type, ==, OBJ8_CMD_GROUP);
	memcpy(pvm, pvm_in, sizeof (pvm));

	for (obj8_cmd_t *subcmd = list_head(&cmd->group.cmds); subcmd != NULL;
	    subcmd = list_next(&cmd->group.cmds, subcmd)) {
		switch (subcmd->type) {
		case OBJ8_CMD_GROUP:
			if (!do_show)
				break;
			obj8_draw_group_cmd(obj, subcmd, groupname, pvm);
			break;
		case OBJ8_CMD_TRIS:
			if (!do_show)
				break;
			if (groupname == NULL ||
			    strcmp(subcmd->tris.group_id, groupname) == 0) {
				if (subcmd->tris.double_sided) {
					glCullFace(GL_FRONT);
					geom_draw(obj, &subcmd->tris, pvm);
					glCullFace(GL_BACK);
				}
				geom_draw(obj, &subcmd->tris, pvm);
			}
			break;
		case OBJ8_CMD_ANIM_HIDE_SHOW: {
			double val = cmd_dr_read(subcmd);

			PROT_BAD_ANIM_VAL(val, subcmd, break);
			if (subcmd->hide_show.val[0] <= val &&
			    subcmd->hide_show.val[1] >= val)
				do_show = subcmd->hide_show.set_val;
			break;
		}
		case OBJ8_CMD_ANIM_ROTATE:
			glm_rotate(pvm, DEG2RAD(rotation_get_angle(subcmd)),
			    (vec3){ subcmd->rotate.axis.x,
			    subcmd->rotate.axis.y, subcmd->rotate.axis.z });
			break;
		case OBJ8_CMD_ANIM_TRANS: {
			double val = cmd_dr_read(subcmd);
			vec3 xlate = {0, 0, 0};

			PROT_BAD_ANIM_VAL(val, subcmd, break);
			if (subcmd->trans.n_pts == 1) {
				/*
				 * single-point translations simply set
				 * position
				 */
				xlate[0] = subcmd->trans.pos[0].x;
				xlate[1] = subcmd->trans.pos[0].y;
				xlate[2] = subcmd->trans.pos[0].z;
			}
			for (size_t i = 0; i + 1 < subcmd->trans.n_pts; i++) {
				double v1 = MIN(subcmd->trans.values[i],
				    subcmd->trans.values[i + 1]);
				double v2 = MAX(subcmd->trans.values[i],
				    subcmd->trans.values[i + 1]);
				if (v1 <= val && val <= v2) {
					double rat = (v2 - v1 != 0.0 ?
					    (val - v1) / (v2 - v1) : 0.0);

					xlate[0] = wavg(subcmd->trans.pos[i].x,
					    subcmd->trans.pos[i + 1].x, rat);
					xlate[1] = wavg(subcmd->trans.pos[i].y,
					    subcmd->trans.pos[i + 1].y, rat);
					xlate[2] = wavg(subcmd->trans.pos[i].z,
					    subcmd->trans.pos[i + 1].z, rat);
					break;
				}
			}

			glm_translate(pvm, xlate);
			break;
		}
		default:
			break;
		}
	}
	GLUTILS_ASSERT_NO_ERROR();
}

static void
setup_arrays(obj8_t *obj, GLuint prog)
{
	if (obj->last_prog != prog) {
		obj->last_prog = prog;
		obj->pos_loc = glGetAttribLocation(prog, "vtx_pos");
		obj->norm_loc = glGetAttribLocation(prog, "vtx_norm");
		obj->tex0_loc = glGetAttribLocation(prog, "vtx_tex0");
		obj->pvm_loc = glGetUniformLocation(prog, "pvm");
		GLUTILS_ASSERT_NO_ERROR();
	}

	glutils_enable_vtx_attr_ptr(obj->pos_loc, 3, GL_FLOAT,
	    GL_FALSE, sizeof (obj8_vtx_t), offsetof(obj8_vtx_t, pos));
	glutils_enable_vtx_attr_ptr(obj->norm_loc, 3, GL_FLOAT,
	    GL_FALSE, sizeof (obj8_vtx_t), offsetof(obj8_vtx_t, norm));
	glutils_enable_vtx_attr_ptr(obj->tex0_loc, 2, GL_FLOAT,
	    GL_FALSE, sizeof (obj8_vtx_t), offsetof(obj8_vtx_t, tex));
	GLUTILS_ASSERT_NO_ERROR();
}

void
obj8_draw_group(obj8_t *obj, const char *groupname, GLuint prog,
    const mat4 pvm_in)
{
	mat4 pvm;

	ASSERT(prog != 0);

	glutils_reset_errors();

	if (!upload_data(obj))
		return;

	glutils_debug_push(0, "obj8_draw_group(%s)",
	    lacf_basename(obj->filename));

	glBindBuffer(GL_ARRAY_BUFFER, obj->vtx_buf);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, obj->idx_buf);

	setup_arrays(obj, prog);

	glm_mat4_mul((vec4 *)pvm_in, obj->matrix, pvm);
	obj8_draw_group_cmd(obj, obj->top, groupname, pvm);

	gl_state_cleanup();
	glutils_disable_vtx_attr_ptr(obj->pos_loc);
	glutils_disable_vtx_attr_ptr(obj->norm_loc);
	glutils_disable_vtx_attr_ptr(obj->tex0_loc);

	glutils_debug_pop();
	GLUTILS_ASSERT_NO_ERROR();
}

/*
 * Applies a pre-transform matrix to all geometry in the OBJ. You can use
 * this when the simple pos_offset parameter in obj8_parse isn't enough.
 * Simply supply a custom transform matrix here and it will be applied
 * instead of the fixed pos_offset. Please note that this completely
 * replaces the old matrix established using pos_offset, so be sure to
 * apply any offseting you passed in obj8_parse to the matrix passed here
 * as well.
 */
void
obj8_set_matrix(obj8_t *obj, mat4 matrix)
{
	dr_t cgY_orig, cgZ_orig;
	mat4 m1;

	fdr_find(&cgY_orig, "sim/aircraft/weight/acf_cgY_original");
	fdr_find(&cgZ_orig, "sim/aircraft/weight/acf_cgZ_original");

	glm_translate_make(m1, (vec3){0,
	    -FEET2MET(dr_getf(&cgY_orig)), -FEET2MET(dr_getf(&cgZ_orig))});
	glm_mat4_mul(matrix, m1, obj->matrix);
}

const char *
obj8_get_filename(const obj8_t *obj)
{
	ASSERT(obj != NULL);
	return (obj->filename);
}
