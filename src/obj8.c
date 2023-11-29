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

#include "librain_glpriv.h"
#include "obj8.h"
#ifdef	DLLMODE
#include "librain.h"
#endif

#define	ANIM_ALLOC_STEP	8
/*
 * After rendering starts, we only try to lookup datarefs for a little
 * while. If within 10 draw cycles the dataref isn't populated, we give
 * up to avoid lags performing costly dataref lookups for datarefs that
 * are likely never going to show up.
 */
#define	MAX_DR_LOOKUPS	10

#define	INVALID_DRSET_IDX	UINT_MAX

TEXSZ_MK_TOKEN(obj8_vtx_buf);
TEXSZ_MK_TOKEN(obj8_idx_buf);

typedef enum {
	OBJ8_CMD_GROUP,
	OBJ8_CMD_TRIS,
	OBJ8_CMD_ANIM_HIDE_SHOW,
	OBJ8_CMD_ANIM_TRANS,
	OBJ8_CMD_ANIM_ROTATE,
	OBJ8_CMD_ATTR_LIGHT_LEVEL,
	OBJ8_CMD_ATTR_DRAW_ENABLE,
	OBJ8_CMD_ATTR_DRAW_DISABLE,
	OBJ8_NUM_CMDS
} obj8_cmd_type_t;

typedef struct {
	unsigned	vtx_off;	/* offset into index table */
	unsigned	n_vtx;		/* number of vertices in geometry */
	char		group_id[32];	/* Contents of X-GROUP-ID attribute */
	bool_t		double_sided;
	unsigned	manip_idx;
	list_node_t	node;
} obj8_geom_t;

typedef struct obj8_cmd_s {
	obj8_cmd_type_t		type;
	struct obj8_cmd_s	*parent;
	unsigned		drset_idx;
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
		struct {
			float	min_val;
			float	max_val;
		} attr_light_level;
		obj8_geom_t	tris;
	};
	list_node_t	list_node;
} obj8_cmd_t;

struct obj8_s {
	char			*filename;
	// Immutable after init
	float			cgY_orig;	// meters
	float			cgZ_orig;	// meters
	/*
	 * These are populated by the loader.
	 */
	char			*tex_filename;
	char			*norm_filename;
	char			*lit_filename;
	obj8_manip_t		*manips;
	unsigned		n_manips;
	unsigned		cap_manips;
	obj8_render_mode_t	render_mode;
	int32_t			render_mode_arg;
	float			light_level_override;
	obj8_drset_t		*drset;
	bool			drset_auto_update;

	GLuint			last_prog;
	/* uniforms */
	GLint			pvm_loc;
	GLint			light_level_loc;
	GLint			manip_idx_loc;
	/* vertex attributes */
	GLint			pos_loc;
	GLint			norm_loc;
	GLint			tex0_loc;
	/*
	 * When using VAOs, it is critical that we NEVER attempt to draw from
	 * a thread other than the one that originally generated the VAO,
	 * because VAOs are thread-local and not shared even between shared
	 * contexts.
	 */
	thread_id_t		upload_thread_id;
	GLuint			vao;
	void			*vtx_table;
	GLuint			vtx_buf;
	unsigned		vtx_cap;
	void			*idx_table;
	GLuint			idx_buf;
	unsigned		idx_cap;
	mat4			*matrix;
	obj8_cmd_t		*top;

	thread_t		loader;
	mutex_t			lock;
	condvar_t		cv;
	bool_t			load_complete;
	bool_t			load_error;
	bool_t			load_stop;
};

typedef struct {
	FILE		*fp;
	vect3_t		pos_offset;
	vect3_t		cg_offset;
	obj8_t		*obj;
} obj8_load_info_t;

typedef struct {
	vec3		pos;
	vec3		norm;
	vec2		tex;
} obj8_vtx_t;

typedef struct {
	unsigned	index;
	int		dr_offset;
	char		dr_name[128];
	bool		dr_found;
	unsigned	dr_lookup_done;
	dr_t		dr;
	float		trig_delta;

	avl_node_t	tree_node;
	list_node_t	list_node;
} drset_dr_t;

static inline bool
use_vaos(void)
{
	return (GLEW_VERSION_3_0);
}

static void
obj8_geom_init(obj8_geom_t *geom, const char *group_id, bool_t double_sided,
    unsigned manip_idx, unsigned off, unsigned len, GLuint vtx_cap,
    const GLuint *idx_table, GLuint idx_cap)
{
	geom->vtx_off = off;
	geom->n_vtx = len;
	strlcpy(geom->group_id, group_id, sizeof (geom->group_id));
	geom->double_sided = double_sided;
	geom->manip_idx = manip_idx;

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
parse_hide_show(obj8_t *obj, bool_t set_val, const char *fmt,
    const char *line, const char *filename, int linenr, obj8_cmd_t *parent)
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
	cmd->drset_idx = obj8_drset_add(obj->drset, dr_name, 0);

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

		cmd->trans.values = safe_realloc(cmd->trans.values,
		    new_cap * sizeof (*cmd->trans.values));
		cmd->trans.pos = safe_realloc(cmd->trans.pos,
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

		cmd->rotate.pts = safe_realloc(cmd->rotate.pts,
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

	return (B_TRUE);
}

static obj8_manip_cursor_t
str2cursor(const char *str)
{
	ASSERT(str != NULL);
	if (strcmp(str, "four_arrows") == 0)
		return (OBJ8_CURSOR_FOUR_ARROWS);
	if (strcmp(str, "hand") == 0)
		return (OBJ8_CURSOR_HAND);
	if (strcmp(str, "button") == 0)
		return (OBJ8_CURSOR_BUTTON);
	if (strcmp(str, "rotate_small") == 0)
		return (OBJ8_CURSOR_ROTATE_SMALL);
	if (strcmp(str, "rotate_small_left") == 0)
		return (OBJ8_CURSOR_ROTATE_SMALL_LEFT);
	if (strcmp(str, "rotate_small_right") == 0)
		return (OBJ8_CURSOR_ROTATE_SMALL_RIGHT);
	if (strcmp(str, "rotate_medium") == 0)
		return (OBJ8_CURSOR_ROTATE_MEDIUM);
	if (strcmp(str, "rotate_medium_left") == 0)
		return (OBJ8_CURSOR_ROTATE_MEDIUM_LEFT);
	if (strcmp(str, "rotate_medium_righT") == 0)
		return (OBJ8_CURSOR_ROTATE_MEDIUM_RIGHT);
	if (strcmp(str, "rotate_large") == 0)
		return (OBJ8_CURSOR_ROTATE_LARGE);
	if (strcmp(str, "rotate_large_left") == 0)
		return (OBJ8_CURSOR_ROTATE_LARGE_LEFT);
	if (strcmp(str, "rotate_large_right") == 0)
		return (OBJ8_CURSOR_ROTATE_LARGE_RIGHT);
	if (strcmp(str, "up_down") == 0)
		return (OBJ8_CURSOR_UP_DOWN);
	if (strcmp(str, "down") == 0)
		return (OBJ8_CURSOR_DOWN);
	if (strcmp(str, "up") == 0)
		return (OBJ8_CURSOR_UP);
	if (strcmp(str, "left_right") == 0)
		return (OBJ8_CURSOR_LEFT_RIGHT);
	if (strcmp(str, "right") == 0)
		return (OBJ8_CURSOR_RIGHT);
	if (strcmp(str, "left") == 0)
		return (OBJ8_CURSOR_LEFT);
	return (OBJ8_CURSOR_ARROW);
}

static obj8_manip_t *
alloc_manip(obj8_t *obj, obj8_manip_type_t type, const char *cursor)
{
	obj8_manip_t *manip;

	ASSERT(obj != NULL);
	ASSERT(cursor != NULL);

	if (obj->n_manips == obj->cap_manips) {
		obj->cap_manips += 32;
		obj->manips = safe_realloc(obj->manips, obj->cap_manips *
		    sizeof (*obj->manips));
	}
	obj->n_manips++;
	manip = &obj->manips[obj->n_manips - 1];
	memset(manip, 0, sizeof (*manip));
	manip->idx = obj->n_manips - 1;
	manip->type = type;
	manip->cursor = str2cursor(cursor);

	return (manip);
}

static unsigned
parse_ATTR_manip_command(const char *line, obj8_t *obj)
{
	obj8_manip_t *manip;
	char cursor[32], cmdname[256];

	ASSERT(line != NULL);
	ASSERT(obj != NULL);

	if (sscanf(line, "ATTR_manip_command %31s %255s", cursor, cmdname) != 2)
		return (-1u);
	manip = alloc_manip(obj, OBJ8_MANIP_COMMAND, cursor);
	manip->cmd = XPLMFindCommand(cmdname);
	strlcpy(manip->cmdname, cmdname, sizeof (manip->cmdname));
	if (manip->cmd == NULL)
		return (-1u);

	return (obj->n_manips - 1);
}

static unsigned
parse_ATTR_manip_toggle(const char *line, obj8_t *obj)
{
	obj8_manip_t *manip;
	char cursor[32], dr_name[256];
	float v1, v2;

	ASSERT(line != NULL);
	ASSERT(obj != NULL);

	if (sscanf(line, "ATTR_manip_toggle %31s %f %f %255s",
	    cursor, &v1, &v2, dr_name) != 4) {
		return (-1u);
	}
	manip = alloc_manip(obj, OBJ8_MANIP_TOGGLE, cursor);
	manip->toggle.drset_idx = obj8_drset_add(obj->drset, dr_name, 0);
	manip->toggle.v1 = v1;
	manip->toggle.v2 = v2;

	return (obj->n_manips - 1);
}

static unsigned
parse_ATTR_manip_noop(obj8_t *obj)
{
	(void)alloc_manip(obj, OBJ8_MANIP_NOOP, "arrow");
	return (obj->n_manips - 1);
}

static unsigned
parse_ATTR_manip_command_knob(const char *line, obj8_t *obj)
{
	obj8_manip_t *manip;
	char cursor[32], pos_cmdname[256], neg_cmdname[256];

	ASSERT(line != NULL);
	ASSERT(obj != NULL);

	if (sscanf(line, "ATTR_manip_command_knob %31s %255s %255s",
	    cursor, pos_cmdname, neg_cmdname) != 3) {
		return (-1u);
	}
	manip = alloc_manip(obj, OBJ8_MANIP_COMMAND_KNOB, cursor);
	manip->cmd_knob.pos_cmd = XPLMFindCommand(pos_cmdname);
	manip->cmd_knob.neg_cmd = XPLMFindCommand(neg_cmdname);
	if (manip->cmd_knob.pos_cmd == NULL ||
	    manip->cmd_knob.neg_cmd == NULL) {
		return (-1u);
	}
	return (obj->n_manips - 1);
}

static unsigned
parse_ATTR_manip_command_axis(const char *line, obj8_t *obj)
{
	obj8_manip_t *manip;
	vect3_t d;
	char cursor[32], pos_cmdname[256], neg_cmdname[256];

	ASSERT(line != NULL);
	ASSERT(obj != NULL);

	if (sscanf(line, "ATTR_manip_command_axis %31s %lf %lf %lf %255s %255s",
	    cursor, &d.x, &d.y, &d.z, pos_cmdname, neg_cmdname) != 6) {
		return (-1u);
	}
	manip = alloc_manip(obj, OBJ8_MANIP_COMMAND_AXIS, cursor);
	manip->cmd_axis.d = d;
	manip->cmd_axis.pos_cmd = XPLMFindCommand(pos_cmdname);
	manip->cmd_axis.neg_cmd = XPLMFindCommand(neg_cmdname);
	if (manip->cmd_axis.pos_cmd == NULL ||
	    manip->cmd_axis.neg_cmd == NULL) {
		return (-1u);
	}
	return (obj->n_manips - 1);
}

static unsigned
parse_ATTR_manip_drag_rotate(const char *line, obj8_t *obj, vect3_t offset)
{
	obj8_manip_t *manip;
	vect3_t xyz, d;
	float angle1, angle2, lift, v1min, v1max, v2min, v2max;
	char cursor[32], dr1_name[256], dr2_name[256];

	ASSERT(line != NULL);
	ASSERT(obj != NULL);

	if (sscanf(line, "ATTR_manip_drag_rotate %31s %lf %lf %lf "
	    "%lf %lf %lf %f %f %f %f %f %f %f %255s %255s", cursor,
	    &xyz.x, &xyz.y, &xyz.z, &d.x, &d.y, &d.z, &angle1, &angle2,
	    &lift, &v1min, &v1max, &v2min, &v2max, dr1_name, dr2_name) != 16) {
		return (-1u);
	}
	manip = alloc_manip(obj, OBJ8_MANIP_DRAG_ROTATE, cursor);
	manip->drag_rot.xyz = vect3_add(offset, xyz);
	manip->drag_rot.dir = d;
	manip->drag_rot.angle1 = angle1;
	manip->drag_rot.angle2 = angle2;
	manip->drag_rot.lift = lift;
	manip->drag_rot.v1min = v1min;
	manip->drag_rot.v1max = v1max;
	manip->drag_rot.v2min = v2min;
	manip->drag_rot.v2max = v2max;
	manip->drag_rot.drset_idx1 = obj8_drset_add(obj->drset, dr1_name, 0);
	manip->drag_rot.drset_idx2 = obj8_drset_add(obj->drset, dr2_name, 0);

	return (obj->n_manips - 1);
}

static unsigned
parse_ATTR_manip_drag_axis(const char *line, obj8_t *obj)
{
	obj8_manip_t *manip;
	float dx, dy, dz, v1, v2;
	char cursor[32], dr_name[256];

	ASSERT(line != NULL);
	ASSERT(obj != NULL);

	if (sscanf(line, "ATTR_manip_drag_axis %31s %f %f %f %f %f %255s",
	    cursor, &dx, &dy, &dz, &v1, &v2, dr_name) != 7) {
		return (-1u);
	}
	manip = alloc_manip(obj, OBJ8_MANIP_DRAG_AXIS, cursor);
	manip->drag_axis.dx = dx;
	manip->drag_axis.dy = dy;
	manip->drag_axis.dz = dz;
	manip->drag_axis.v1 = v1;
	manip->drag_axis.v2 = v2;
	manip->drag_axis.drset_idx = obj8_drset_add(obj->drset, dr_name, 0);

	return (obj->n_manips - 1);
}

static unsigned
parse_ATTR_manip_drag_xy(const char *line, obj8_t *obj)
{
	obj8_manip_t *manip;
	float dx, dy, v1min, v1max, v2min, v2max;
	char cursor[32], dr1_name[256], dr2_name[256];

	ASSERT(line != NULL);
	ASSERT(obj != NULL);

	if (sscanf(line, "ATTR_manip_drag_xy %31s %f %f %f %f %f %f "
	    "%255s %255s", cursor, &dx, &dy, &v1min, &v1max, &v2min,
	    &v2max, dr1_name, dr2_name) != 9) {
		return (-1u);
	}
	manip = alloc_manip(obj, OBJ8_MANIP_DRAG_XY, cursor);
	manip->drag_xy.dx = dx;
	manip->drag_xy.dy = dy;
	manip->drag_xy.v1min = v1min;
	manip->drag_xy.v1max = v1max;
	manip->drag_xy.v2min = v2min;
	manip->drag_xy.v2max = v2max;
	manip->drag_rot.drset_idx1 = obj8_drset_add(obj->drset, dr1_name, 0);
	manip->drag_rot.drset_idx2 = obj8_drset_add(obj->drset, dr2_name, 0);

	return (obj->n_manips - 1);
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
	unsigned	cur_manip = -1u;

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
	glm_translate_make(*obj->matrix, (vec3){offset.x, offset.y, offset.z});

	for (int linenr = 1; lacf_getline(&line, &cap, fp) > 0 &&
	    !obj->load_stop; linenr++) {
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
			    &vtx->pos[0], &vtx->pos[1], &vtx->pos[2],
			    &vtx->norm[0], &vtx->norm[1], &vtx->norm[2],
			    &vtx->tex[0], &vtx->tex[1]) != 8) {
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
			    cur_manip, off, len, vtx_cap, idx_table, idx_cap);
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
			if (!parse_hide_show(obj, B_TRUE,
			    "ANIM_show %lf %lf %255s",
			    line, filename, linenr, cur_cmd))
				goto errout;
		} else if (strncmp(line, "ANIM_hide", 9) == 0) {
			if (!parse_hide_show(obj, B_FALSE,
			    "ANIM_hide %lf %lf %255s",
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
			cmd->drset_idx = obj8_drset_add(obj->drset, dr_name, 0);
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
			cmd->drset_idx = obj8_drset_add(obj->drset, dr_name, 0);
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
			    safe_calloc(sizeof (*cmd->trans.values), 2);
			cmd->trans.pos = safe_calloc(sizeof (*cmd->trans.pos),
			    2);
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
			cmd->drset_idx = obj8_drset_add(obj->drset, dr_name, 0);
		} else if (strncmp(line, "ANIM_rotate", 11) == 0) {
			char dr_name[256] = { 0 };
			obj8_cmd_t *cmd;

			cmd = obj8_cmd_alloc(OBJ8_CMD_ANIM_ROTATE, cur_cmd);
			cmd->rotate.n_pts = 2;
			cmd->rotate.n_pts_cap = 2;
			cmd->rotate.pts = safe_calloc(sizeof (*cmd->rotate.pts),
			    2);
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
			cmd->drset_idx = obj8_drset_add(obj->drset, dr_name, 0);
		} else if (strncmp(line, "ATTR_light_level", 16) == 0) {
			char dr_name[256] = { 0 };
			float min_val, max_val;
			/*
			 * The dataref name is optional, so skip creating
			 * the command if it is empty.
			 */
			obj8_cmd_t *cmd = obj8_cmd_alloc(
			    OBJ8_CMD_ATTR_LIGHT_LEVEL, cur_cmd);
			if (sscanf(line, "ATTR_light_level %f %f %255s",
			    &min_val, &max_val, dr_name) == 3) {
				cmd->attr_light_level.min_val = min_val;
				cmd->attr_light_level.max_val = max_val;
				cmd->drset_idx = obj8_drset_add(
				    obj->drset, dr_name, 0.02);
			} else {
				cmd->drset_idx = obj8_drset_add(
				    obj->drset, NULL, 0);
			}
		} else if (strncmp(line, "ATTR_draw_enable", 16) == 0) {
			(void)obj8_cmd_alloc(OBJ8_CMD_ATTR_DRAW_ENABLE,
			    cur_cmd);
		} else if (strncmp(line, "ATTR_draw_disable", 17) == 0) {
			(void)obj8_cmd_alloc(OBJ8_CMD_ATTR_DRAW_DISABLE,
			    cur_cmd);
		} else if (strncmp(line, "ATTR_manip_none", 15) == 0) {
			cur_manip = -1;
		} else if (strncmp(line, "ATTR_manip_command_axis", 23) == 0) {
			cur_manip = parse_ATTR_manip_command_axis(line, obj);
		} else if (strncmp(line, "ATTR_manip_command_knob", 23) == 0) {
			cur_manip = parse_ATTR_manip_command_knob(line, obj);
		} else if (strncmp(line, "ATTR_manip_command", 18) == 0) {
			cur_manip = parse_ATTR_manip_command(line, obj);
		} else if (strncmp(line, "ATTR_manip_drag_rotate", 22) == 0) {
			cur_manip = parse_ATTR_manip_drag_rotate(line, obj,
			    offset);
		} else if (strncmp(line, "ATTR_manip_drag_axis", 20) == 0) {
			cur_manip = parse_ATTR_manip_drag_axis(line, obj);
		} else if (strncmp(line, "ATTR_manip_drag_xy", 18) == 0) {
			cur_manip = parse_ATTR_manip_drag_xy(line, obj);
		} else if (strncmp(line, "ATTR_manip_toggle", 17) == 0) {
			cur_manip = parse_ATTR_manip_toggle(line, obj);
		} else if (strncmp(line, "ATTR_manip_noop", 15) == 0) {
			cur_manip = parse_ATTR_manip_noop(obj);
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
		} else if (strncmp(line, "TEXTURE_NORMAL", 14) == 0) {
			char buf[128];
			if (sscanf(line, "TEXTURE_NORMAL %127s", buf) == 1) {
				obj->norm_filename = path_last_comp_subst(
				    obj->filename, buf);
			}
		} else if (strncmp(line, "TEXTURE_LIT", 11) == 0) {
			char buf[128];
			if (sscanf(line, "TEXTURE_LIT %127s", buf) == 1) {
				obj->lit_filename = path_last_comp_subst(
				    obj->filename, buf);
			}
		} else if (strncmp(line, "TEXTURE", 7) == 0) {
			char buf[128];
			if (sscanf(line, "TEXTURE %127s", buf) == 1) {
				obj->tex_filename = path_last_comp_subst(
				    obj->filename, buf);
			}
		}
	}

	obj->vtx_table = vtx_table;
	obj->idx_table = idx_table;

	obj->vtx_cap = vtx_cap;
	obj->idx_cap = idx_cap;

	free(line);

	fclose(info->fp);
	free(info);

	obj8_drset_mark_complete(obj->drset);

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
	obj8_t *obj = safe_calloc(1, sizeof (*obj));
	{
		dr_t cgY_orig, cgZ_orig;
		fdr_find(&cgY_orig, "sim/aircraft/weight/acf_cgY_original");
		fdr_find(&cgZ_orig, "sim/aircraft/weight/acf_cgZ_original");
		obj->cgY_orig = FEET2MET(dr_getf(&cgY_orig));
		obj->cgZ_orig = FEET2MET(dr_getf(&cgZ_orig));
	}
	obj->matrix = safe_aligned_calloc(MAT4_ALLOC_ALIGN, 1, sizeof (mat4));
	mutex_init(&obj->lock);
	cv_init(&obj->cv);
	obj->filename = safe_strdup(filename);
	obj->light_level_override = NAN;
	obj->drset_auto_update = true;
	obj->drset = obj8_drset_new();

	obj->pos_loc = -1;
	obj->norm_loc = -1;
	obj->tex0_loc = -1;

	obj8_load_info_t *info = safe_calloc(1, sizeof (*info));
	info->fp = fp;
	info->pos_offset = pos_offset;
	info->obj = obj;
	info->cg_offset = VECT3(0, -obj->cgY_orig, -obj->cgZ_orig);

	VERIFY(thread_create(&obj->loader, obj8_parse_worker, info));

	return (obj);
}

static inline void
wait_load_complete(obj8_t *obj)
{
	if (!obj->load_complete) {
		mutex_enter(&obj->lock);
		while (!obj->load_complete)
			cv_wait(&obj->cv, &obj->lock);
		mutex_exit(&obj->lock);
	}
}

static bool_t
upload_data(obj8_t *obj)
{
	if (!obj->load_complete)
		return (B_FALSE);
	wait_load_complete(obj);
	/*
	 * Once the initial data load is complete, upload the tables and
	 * dispose of the in-memory copies as they are no longer needed.
	 */
	if (obj->vtx_buf == 0 && !obj->load_error) {
		ASSERT(obj->vtx_table != NULL);
		ASSERT(obj->idx_table != NULL);

		obj->upload_thread_id = curthread_id;
		if (use_vaos()) {
			ASSERT0(obj->vao);
			glGenVertexArrays(1, &obj->vao);
			if (obj->vao != 0) {
				glBindVertexArray(obj->vao);
			} else {
				// On macOS, even though GLEW can find the
				// glGenVertexArrays function, if we're in
				// a compatibility context, this function
				// simply errors out. So just swallow the
				// error and carry on without a VAO.
				glutils_reset_errors();
			}
		}
		glGenBuffers(1, &obj->vtx_buf);
		glBindBuffer(GL_ARRAY_BUFFER, obj->vtx_buf);
		if (GLEW_ARB_buffer_storage) {
			glBufferStorage(GL_ARRAY_BUFFER, obj->vtx_cap *
			    sizeof (obj8_vtx_t), obj->vtx_table, 0);
		} else {
			glBufferData(GL_ARRAY_BUFFER, obj->vtx_cap *
			    sizeof (obj8_vtx_t), obj->vtx_table,
			    GL_STATIC_DRAW);
		}
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		IF_TEXSZ(TEXSZ_ALLOC_BYTES_INSTANCE(obj8_vtx_buf, obj,
		    obj->filename, 0, obj->vtx_cap * sizeof (obj8_vtx_t)));
		free(obj->vtx_table);
		obj->vtx_table = NULL;

		glGenBuffers(1, &obj->idx_buf);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, obj->idx_buf);
		if (GLEW_ARB_buffer_storage) {
			glBufferStorage(GL_ELEMENT_ARRAY_BUFFER, obj->idx_cap *
			    sizeof (GLuint), obj->idx_table, 0);
		} else {
			glBufferData(GL_ELEMENT_ARRAY_BUFFER, obj->idx_cap *
			    sizeof (GLuint), obj->idx_table, GL_STATIC_DRAW);
		}
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

#ifdef	DLLMODE
	/*
	 * This is only required in DLL mode in order to get GLEw initialized.
	 */
	if (!librain_glob_init())
		return (NULL);
#endif	/* defined(DLLMODE) */
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
	case OBJ8_CMD_ATTR_LIGHT_LEVEL:
	case OBJ8_CMD_ATTR_DRAW_ENABLE:
	case OBJ8_CMD_ATTR_DRAW_DISABLE:
		break;
	default:
		VERIFY_FAIL();
	}
	free(cmd);
}

bool
obj8_needs_upload(const obj8_t *obj)
{
	ASSERT(obj != NULL);
	return (obj->load_complete && !obj->load_error && obj->vtx_buf == 0);
}

void
obj8_free(obj8_t *obj)
{
	ASSERT(obj != NULL);

	obj8_cmd_free(obj->top);

	obj->load_stop = B_TRUE;
	thread_join(&obj->loader);
	mutex_destroy(&obj->lock);
	cv_destroy(&obj->cv);

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
	if (obj->vao != 0) {
		glDeleteVertexArrays(1, &obj->vao);
	}
	free(obj->idx_table);
	free(obj->filename);
	free(obj->tex_filename);
	free(obj->norm_filename);
	free(obj->lit_filename);

	free(obj->manips);

	obj8_drset_destroy(obj->drset);

	aligned_free(obj->matrix);
	ZERO_FREE(obj);
}

static inline float
cmd_dr_read(obj8_cmd_t *cmd, const float *dr_values)
{
	if (cmd->drset_idx != INVALID_DRSET_IDX) {
		return (dr_values[cmd->drset_idx]);
	} else {
		return (0);
	}
}

static void
geom_draw(const obj8_t *obj, const obj8_geom_t *geom, const mat4 pvm)
{
	glUniformMatrix4fv(obj->pvm_loc, 1, GL_FALSE, (void *)pvm);
	glUniform1f(obj->manip_idx_loc, geom->manip_idx);
	glDrawElements(GL_TRIANGLES, geom->n_vtx, GL_UNSIGNED_INT,
	    (void *)(geom->vtx_off * sizeof (GLuint)));
}

static inline double
anim_extrapolate_1D(double *v_p, vect2_t v1, vect2_t v2)
{
	if ((v1.x < v2.x && (*v_p < v1.x || *v_p > v2.x)) ||
	    (v1.x > v2.x && (*v_p > v1.x || *v_p < v2.x))) {
		*v_p = fx_lin(*v_p, v1.x, v1.y, v2.x, v2.y);
		ASSERT(!isnan(*v_p));
		return (B_TRUE);
	}
	return (B_FALSE);
}

static double
rotation_get_angle(const obj8_t *obj, obj8_cmd_t *cmd, const float *dr_values)
{
	double val = cmd_dr_read(cmd, dr_values);
	size_t n = cmd->rotate.n_pts;

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

	logMsg("Something went really wrong during animation of %s with value "
	    "%f", obj8_drset_get_dr_name(obj->drset, cmd->drset_idx), val);

	VERIFY_FAIL();
}

static inline void
handle_cmd_anim_rotate(const obj8_t *obj, obj8_cmd_t *subcmd, mat4 pvm,
    const float *dr_values)
{
	glm_rotate(pvm, DEG2RAD(rotation_get_angle(obj, subcmd, dr_values)),
	    (vec3){ subcmd->rotate.axis.x,
	    subcmd->rotate.axis.y, subcmd->rotate.axis.z });
}

static void
handle_cmd_anim_trans(obj8_cmd_t *subcmd, mat4 pvm, const float *dr_values)
{
	double val;
	vec3 xlate = {0, 0, 0};

	ASSERT(subcmd != NULL);
	ASSERT(pvm != NULL);
	val = cmd_dr_read(subcmd, dr_values);

	if (subcmd->trans.n_pts == 1) {
		/*
		 * single-point translations simply set position
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
}

static inline bool
render_mode_is_manip_only(obj8_render_mode_t mode)
{
	return (mode == OBJ8_RENDER_MODE_MANIP_ONLY ||
	    mode == OBJ8_RENDER_MODE_MANIP_ONLY_ONE);
}

void
obj8_draw_group_cmd(const obj8_t *obj, obj8_cmd_t *cmd, const char *groupname,
    const mat4 pvm_in, const float *dr_values)
{
	bool_t hide = B_FALSE, do_draw = B_TRUE;
	mat4 pvm;

	ASSERT(obj != NULL);
	ASSERT(cmd != NULL);
	ASSERT3U(cmd->type, ==, OBJ8_CMD_GROUP);
	memcpy(pvm, pvm_in, sizeof (pvm));

	for (obj8_cmd_t *subcmd = list_head(&cmd->group.cmds); subcmd != NULL;
	    subcmd = list_next(&cmd->group.cmds, subcmd)) {
		switch (subcmd->type) {
		case OBJ8_CMD_GROUP:
			if (hide || (!do_draw &&
			    !render_mode_is_manip_only(obj->render_mode))) {
				break;
			}
			obj8_draw_group_cmd(obj, subcmd, groupname, pvm,
			    dr_values);
			break;
		case OBJ8_CMD_TRIS:
			/* Don't draw if we're hidden */
			if (hide)
				break;
			if (obj->render_mode == OBJ8_RENDER_MODE_NORM) {
				/*
				 * If we're in normal rendering mode, don't
				 * draw if ATTR_draw_disable is active.
				 */
				if (!do_draw)
					break;
			} else if (obj->render_mode ==
			    OBJ8_RENDER_MODE_MANIP_ONLY) {
				/*
				 * If we're in manipulator drawing mode,
				 * don't draw if this isn't a manipulator.
				 */
				if (subcmd->tris.manip_idx == -1u)
					break;
			} else {
				ASSERT3U(obj->render_mode, ==,
				    OBJ8_RENDER_MODE_MANIP_ONLY_ONE);
				/*
				 * If we're in single-manipulator drawing mode,
				 * don't draw if this isn't a manipulator,
				 * or the manipulator index doesn't match the
				 * one manipulator we do want to draw.
				 */
				if (subcmd->tris.manip_idx == -1u ||
				    (int)subcmd->tris.manip_idx !=
				    obj->render_mode_arg) {
					break;
				}
			}
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
			double val = cmd_dr_read(subcmd, dr_values);

			if (subcmd->hide_show.val[0] <= val &&
			    subcmd->hide_show.val[1] >= val)
				hide = !subcmd->hide_show.set_val;
			break;
		}
		case OBJ8_CMD_ANIM_ROTATE:
			handle_cmd_anim_rotate(obj, subcmd, pvm, dr_values);
			break;
		case OBJ8_CMD_ANIM_TRANS:
			handle_cmd_anim_trans(subcmd, pvm, dr_values);
			break;
		case OBJ8_CMD_ATTR_LIGHT_LEVEL:
			if (isnan(obj->light_level_override)) {
				float raw = cmd_dr_read(subcmd, dr_values);
				float value = 0;
				if (subcmd->attr_light_level.min_val <
				    subcmd->attr_light_level.max_val) {
					value = iter_fract(raw,
					    subcmd->attr_light_level.min_val,
					    subcmd->attr_light_level.max_val,
					    true);
				}
				glUniform1f(obj->light_level_loc, value);
			}
			break;
		case OBJ8_CMD_ATTR_DRAW_ENABLE:
			do_draw = B_TRUE;
			break;
		case OBJ8_CMD_ATTR_DRAW_DISABLE:
			do_draw = B_FALSE;
			break;
		default:
			break;
		}
	}
}

static inline void
enable_vtx_attr_ptrs(const obj8_t *obj)
{
	glutils_enable_vtx_attr_ptr(obj->pos_loc, 3, GL_FLOAT,
	    GL_FALSE, sizeof (obj8_vtx_t), offsetof(obj8_vtx_t, pos));
	glutils_enable_vtx_attr_ptr(obj->norm_loc, 3, GL_FLOAT,
	    GL_FALSE, sizeof (obj8_vtx_t), offsetof(obj8_vtx_t, norm));
	glutils_enable_vtx_attr_ptr(obj->tex0_loc, 2, GL_FLOAT,
	    GL_FALSE, sizeof (obj8_vtx_t), offsetof(obj8_vtx_t, tex));
}

static inline void
disable_vtx_attr_ptrs(const obj8_t *obj)
{
	glutils_disable_vtx_attr_ptr(obj->pos_loc);
	glutils_disable_vtx_attr_ptr(obj->norm_loc);
	glutils_disable_vtx_attr_ptr(obj->tex0_loc);
}

static void
setup_arrays(obj8_t *obj, GLuint prog)
{
	if (obj->last_prog != prog) {
		// if using VAOs, disable old set first
		if (obj->vao != 0) {
			disable_vtx_attr_ptrs(obj);
		}
		obj->last_prog = prog;
		/* uniforms */
		obj->pvm_loc = glGetUniformLocation(prog, "pvm");
		obj->light_level_loc = glGetUniformLocation(prog,
		    "ATTR_light_level");
		obj->manip_idx_loc = glGetUniformLocation(prog, "manip_idx");
		/* vertex attributes */
		obj->pos_loc = glGetAttribLocation(prog, "vtx_pos");
		obj->norm_loc = glGetAttribLocation(prog, "vtx_norm");
		obj->tex0_loc = glGetAttribLocation(prog, "vtx_tex0");
		// enable new set
		enable_vtx_attr_ptrs(obj);
	} else if (obj->vao == 0) {
		// Only re-enable the known set of pointers if we're not
		// using VAOs. Otherwise the array bindings will get
		// re-established when rebinding the VAO.
		enable_vtx_attr_ptrs(obj);
	}
}

void
obj8_draw_group(obj8_t *obj, const char *groupname, GLuint prog,
    const mat4 pvm_in)
{
	mat4 pvm;

	ASSERT(prog != 0);

	if (!upload_data(obj))
		return;

	if (obj->drset_auto_update)
		(void)obj8_drset_update(obj->drset);

	enum { MAX_STACK_DRS = 128 };
	float dr_values_stack[MAX_STACK_DRS];
	size_t n_drs = obj8_drset_get_all(obj->drset, NULL, 0);
	float *dr_values;
	if (n_drs > ARRAY_NUM_ELEM(dr_values_stack)) {
		dr_values = safe_malloc(n_drs * sizeof (*dr_values));
	} else {
		dr_values = dr_values_stack;
	}
	obj8_drset_get_all(obj->drset, dr_values, n_drs);

	glutils_debug_push(0, "obj8_draw_group(%s)",
	    lacf_basename(obj->filename));
#if	APL
	/*
	 * Leaving this on on MacOS breaks glDrawElements and makes it
	 * perform horribly.
	 */
	glDisableClientState(GL_VERTEX_ARRAY);
#endif	/* APL */
	if (obj->vao != 0) {
		ASSERT(thread_equal(curthread_id, obj->upload_thread_id));
		glBindVertexArray(obj->vao);
	}
	glBindBuffer(GL_ARRAY_BUFFER, obj->vtx_buf);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, obj->idx_buf);

	setup_arrays(obj, prog);

	if (!isnan(obj->light_level_override))
		glUniform1f(obj->light_level_loc, obj->light_level_override);
	else
		glUniform1f(obj->light_level_loc, 0);
	glm_mat4_mul((vec4 *)pvm_in, *obj->matrix, pvm);
	obj8_draw_group_cmd(obj, obj->top, groupname, pvm, dr_values);

	if (obj->vao != 0) {
		glBindVertexArray(0);
	} else {
		disable_vtx_attr_ptrs(obj);
	}
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	glutils_debug_pop();

	GLUTILS_ASSERT_NO_ERROR();

	if (n_drs > ARRAY_NUM_ELEM(dr_values_stack)) {
		free(dr_values);
	}
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
	mat4 m1;

	ASSERT(obj != NULL);

	glm_translate_make(m1, (vec3){0, -obj->cgY_orig, -obj->cgZ_orig});
	glm_mat4_mul(matrix, m1, *obj->matrix);
}

obj8_render_mode_t
obj8_get_render_mode(const obj8_t *obj)
{
	ASSERT(obj != NULL);
	return (obj->render_mode);
}

void
obj8_set_render_mode(obj8_t *obj, obj8_render_mode_t mode)
{
	obj8_set_render_mode2(obj, mode, 0);
}

void
obj8_set_render_mode2(obj8_t *obj, obj8_render_mode_t mode, int32_t arg)
{
	ASSERT(obj != NULL);
	ASSERT(mode == OBJ8_RENDER_MODE_NORM ||
	    mode == OBJ8_RENDER_MODE_MANIP_ONLY);
	obj->render_mode = mode;
	obj->render_mode_arg = arg;
}

unsigned
obj8_get_num_manips(const obj8_t *obj)
{
	ASSERT(obj != NULL);
	if (!obj->load_complete)
		return (0);
	return (obj->n_manips);
}

const obj8_manip_t *
obj8_get_manip(const obj8_t *obj, unsigned idx)
{
	ASSERT(obj != NULL);
	ASSERT3U(idx, <, obj->n_manips);
	return (&obj->manips[idx]);
}

const char *
obj8_get_filename(const obj8_t *obj)
{
	ASSERT(obj != NULL);
	return (obj->filename);
}

const char *
obj8_get_tex_filename(const obj8_t *obj, bool wait_load)
{
	ASSERT(obj != NULL);
	if (wait_load)
		wait_load_complete((obj8_t *)obj);
	return (obj->tex_filename);
}

const char *
obj8_get_norm_filename(const obj8_t *obj, bool wait_load)
{
	ASSERT(obj != NULL);
	if (wait_load)
		wait_load_complete((obj8_t *)obj);
	return (obj->norm_filename);
}

const char *
obj8_get_lit_filename(const obj8_t *obj, bool wait_load)
{
	ASSERT(obj != NULL);
	if (wait_load)
		wait_load_complete((obj8_t *)obj);
	return (obj->lit_filename);
}

void
obj8_set_light_level_override(obj8_t *obj, float value)
{
	ASSERT(obj != NULL);
	obj->light_level_override = value;
}

float
obj8_get_light_level_override(const obj8_t *obj)
{
	ASSERT(obj != NULL);
	return (obj->light_level_override);
}

void
obj8_set_drset_auto_update(obj8_t *obj, bool flag)
{
	ASSERT(obj != NULL);
	obj->drset_auto_update = flag;
}

bool
obj8_get_drset_auto_update(const obj8_t *obj)
{
	ASSERT(obj != NULL);
	return (obj->drset_auto_update);
}

obj8_drset_t *
obj8_get_drset(const obj8_t *obj)
{
	ASSERT(obj != NULL);
	return (obj->drset);
}

static int
drset_dr_compar(const void *a, const void *b)
{
	const drset_dr_t *dr_a = a, *dr_b = b;
	int res = strcmp(dr_a->dr_name, dr_b->dr_name);

	if (res < 0)
		return (-1);
	if (res > 0)
		return (1);
	return (0);
}

obj8_drset_t *
obj8_drset_new(void)
{
	obj8_drset_t *drset = safe_calloc(1, sizeof (*drset));
	avl_create(&drset->tree, drset_dr_compar, sizeof (drset_dr_t),
	    offsetof(drset_dr_t, tree_node));
	list_create(&drset->list, sizeof (drset_dr_t),
	    offsetof(drset_dr_t, list_node));
	mutex_init(&drset->lock);
	return (drset);
}

void
obj8_drset_destroy(obj8_drset_t *drset)
{
	void *cookie = NULL;
	drset_dr_t *dr;

	if (drset == NULL)
		return;
	/*
	 * Nodes are held in the list, so just destroy the tree quickly.
	 */
	while (avl_destroy_nodes(&drset->tree, &cookie) != NULL)
		;
	avl_destroy(&drset->tree);
	while ((dr = list_remove_head(&drset->list)) != NULL)
		free(dr);
	list_destroy(&drset->list);
	mutex_destroy(&drset->lock);
	free(drset->values);

	free(drset);
}

unsigned
obj8_drset_add(obj8_drset_t *drset, const char *name, float trig_delta)
{
	drset_dr_t srch = {};
	avl_index_t where;
	drset_dr_t *dr;

	ASSERT(drset != NULL);
	ASSERT(!drset->complete);
	/* name can be NULL */
	if (name == NULL) {
		return (INVALID_DRSET_IDX);
	}

	strlcpy(srch.dr_name, name, sizeof (srch.dr_name));
	dr = avl_find(&drset->tree, &srch, &where);
	if (dr == NULL) {
		dr = safe_calloc(1, sizeof (*dr));
		strlcpy(dr->dr_name, name, sizeof (dr->dr_name));
		dr->index = drset->n_drs++;
		dr->trig_delta = trig_delta;
		avl_insert(&drset->tree, dr, where);
		list_insert_tail(&drset->list, dr);
	} else {
		dr->trig_delta = MIN(dr->trig_delta, trig_delta);
	}
	return (dr->index);
}

void
obj8_drset_mark_complete(obj8_drset_t *drset)
{
	ASSERT(drset != NULL);
	drset->values = safe_calloc(drset->n_drs, sizeof (*drset->values));
	drset->trig_deltas = safe_calloc(drset->n_drs,
	    sizeof (*drset->trig_deltas));
	for (const drset_dr_t *dr = list_head(&drset->list); dr != NULL;
	    dr = list_next(&drset->list, dr)) {
		drset->trig_deltas[dr->index] = dr->trig_delta;
	}
	drset->complete = true;
}

static bool
find_dr_with_offset(char *dr_name, dr_t *dr, int *offset)
{
	char *bracket;

	if (dr_find(dr, "%s", dr_name)) {
		*offset = -1;
		return (true);
	}
	bracket = strrchr(dr_name, '[');
	if (bracket != NULL) {
		int cap;

		*bracket = 0;
		if (!dr_find(dr, "%s", dr_name))
			return (false);
		cap = dr_getvf32(dr, NULL, 0, 0);
		if (cap == 0)
			return (false);
		*offset = clampi(atoi(&bracket[1]), 0, cap - 1);
		return (true);
	}

	return (false);
}

static inline float
drset_dr_updatef(drset_dr_t *dr)
{
	float v;

	if (COND_UNLIKELY(!dr->dr_found)) {
		if (COND_LIKELY(dr->dr_lookup_done > MAX_DR_LOOKUPS))
			return (0);
		dr->dr_lookup_done++;
		if (!find_dr_with_offset(dr->dr_name, &dr->dr,
		    &dr->dr_offset)) {
			return (0);
		}
		dr->dr_found = true;
	}
	if (dr->dr_offset > 0)
		dr_getvf32(&dr->dr, &v, dr->dr_offset, 1);
	else
		v = dr_getf(&dr->dr);
	if (COND_UNLIKELY(!isfinite(v))) {
		logMsg("Bad animation dataref %s = %f. Bailing out. "
		    "Report this as a bug and attach the log file.",
		    dr->dr_name, v);
		return (0);
	}

	return (v);
}

static bool
check_drs_have_changed(const obj8_drset_t *drset, const float *new_vals)
{
	ASSERT(drset != NULL);
	ASSERT_MUTEX_HELD(&drset->lock);
	ASSERT(new_vals != NULL);

	for (unsigned i = 0; i < drset->n_drs; i++) {
		if (fabs(drset->values[i] - new_vals[i]) >
		    drset->trig_deltas[i]) {
			return (true);
		}
	}
	return (false);
}

bool
obj8_drset_update(obj8_drset_t *drset)
{
	unsigned idx;
	float *vals;

	ASSERT(drset != NULL);

	if (!drset->complete)
		return (false);

	vals = safe_malloc(drset->n_drs * sizeof (*vals));
	idx = 0;
	ASSERT3U(list_count(&drset->list), ==, drset->n_drs);
	for (drset_dr_t *dr = list_head(&drset->list); dr != NULL;
	    dr = list_next(&drset->list, dr), idx++) {
		vals[idx] = drset_dr_updatef(dr);
	}
	ASSERT(drset->values != NULL);
	mutex_enter(&drset->lock);
	if (!check_drs_have_changed(drset, vals)) {
		mutex_exit(&drset->lock);
		free(vals);
		return (false);
	} else {
		free(drset->values);
		drset->values = vals;
		mutex_exit(&drset->lock);
		return (true);
	}
}

size_t
obj8_drset_get_all(const obj8_drset_t *drset, float *out_values, size_t cap)
{
	ASSERT(drset != NULL);
	ASSERT(drset->complete);
	ASSERT(out_values != NULL || cap == 0);

	mutex_enter((mutex_t *)&drset->lock);
	memcpy(out_values, drset->values, MIN(cap, drset->n_drs) *
	    sizeof (*out_values));
	mutex_exit((mutex_t *)&drset->lock);

	return (drset->n_drs);
}

dr_t *
obj8_drset_get_dr(const obj8_drset_t *drset, unsigned idx)
{
	drset_dr_t *dr;
	ASSERT(drset != NULL);
	if (idx == INVALID_DRSET_IDX) {
		return (NULL);
	}
	ASSERT3U(idx, <, drset->n_drs);
	dr = list_get_i(&drset->list, idx);
	return (dr->dr_found ? &dr->dr : NULL);
}

const char *
obj8_drset_get_dr_name(const obj8_drset_t *drset, unsigned idx)
{
	drset_dr_t *dr;
	ASSERT(drset != NULL);
	if (idx == INVALID_DRSET_IDX) {
		return ("<NULL>");
	}
	ASSERT3U(idx, <, drset->n_drs);
	dr = list_get_i(&drset->list, idx);
	return (dr->dr_name);
}
