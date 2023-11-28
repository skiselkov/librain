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

#ifndef	_OBJ8_H_
#define	_OBJ8_H_

#include <stdio.h>

#include <XPLMUtilities.h>

#include <acfutils/avl.h>
#include <acfutils/dr.h>
#include <acfutils/geom.h>
#include <acfutils/glew.h>
#include <acfutils/list.h>
#include <acfutils/thread.h>

#include <cglm/cglm.h>

#include "librain_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct obj8_s obj8_t;

typedef struct obj8_cmd_s obj8_cmd_t;

typedef enum {
	OBJ8_MANIP_AXIS_KNOB,
	OBJ8_MANIP_COMMAND,
	OBJ8_MANIP_COMMAND_AXIS,
	OBJ8_MANIP_COMMAND_KNOB,
	OBJ8_MANIP_COMMAND_SWITCH_LR,
	OBJ8_MANIP_COMMAND_SWITCH_UD,
	OBJ8_MANIP_DRAG_AXIS,
	OBJ8_MANIP_DRAG_ROTATE,
	OBJ8_MANIP_DRAG_XY,
	OBJ8_MANIP_TOGGLE,
	OBJ8_MANIP_PUSH,
	OBJ8_MANIP_NOOP,
	OBJ8_MANIP_COMMAND_SWITCH_LR2,
	OBJ8_MANIP_COMMAND_SWITCH_UD2
} obj8_manip_type_t;

typedef enum {
	OBJ8_CURSOR_FOUR_ARROWS,
	OBJ8_CURSOR_HAND,
	OBJ8_CURSOR_BUTTON,
	OBJ8_CURSOR_ROTATE_SMALL,
	OBJ8_CURSOR_ROTATE_SMALL_LEFT,
	OBJ8_CURSOR_ROTATE_SMALL_RIGHT,
	OBJ8_CURSOR_ROTATE_MEDIUM,
	OBJ8_CURSOR_ROTATE_MEDIUM_LEFT,
	OBJ8_CURSOR_ROTATE_MEDIUM_RIGHT,
	OBJ8_CURSOR_ROTATE_LARGE,
	OBJ8_CURSOR_ROTATE_LARGE_LEFT,
	OBJ8_CURSOR_ROTATE_LARGE_RIGHT,
	OBJ8_CURSOR_UP_DOWN,
	OBJ8_CURSOR_DOWN,
	OBJ8_CURSOR_UP,
	OBJ8_CURSOR_LEFT_RIGHT,
	OBJ8_CURSOR_RIGHT,
	OBJ8_CURSOR_LEFT,
	OBJ8_CURSOR_ARROW
} obj8_manip_cursor_t;

typedef struct {
	unsigned		idx;
	unsigned		cmdidx;
	obj8_manip_type_t	type;
	obj8_manip_cursor_t	cursor;
	char			cmdname[128];
	union {
		struct {
			float		min, max;
			float		d_click, d_hold;
			dr_t		dr;
			int         dr_offset;
		} manip_axis_knob;
		XPLMCommandRef		cmd;
		struct {
			vect3_t		d;
			XPLMCommandRef	pos_cmd;
			XPLMCommandRef	neg_cmd;
		} cmd_axis;
		struct {
			XPLMCommandRef	pos_cmd;
			XPLMCommandRef	neg_cmd;
		} cmd_knob;
		struct {
			XPLMCommandRef	pos_cmd;
			XPLMCommandRef	neg_cmd;
		} cmd_sw;
		XPLMCommandRef	cmd_sw2;
		struct {
			float		dx, dy, dz;
			float		v1, v2;
			unsigned	drset_idx;
		} drag_axis;
		struct {
			vect3_t		xyz;
			vect3_t		dir;
			float		angle1, angle2;
			float		lift;
			float		v1min, v1max;
			float		v2min, v2max;
			unsigned	drset_idx1, drset_idx2;
		} drag_rot;
		struct {
			float		dx, dy;
			float		v1min, v1max;
			float		v2min, v2max;
			unsigned	drset_idx1, drset_idx2;
		} drag_xy;
		struct {
			unsigned	drset_idx;
			float		v1, v2;
		} toggle;
	};
} obj8_manip_t;

typedef enum {
	OBJ8_RENDER_MODE_NORM,
	OBJ8_RENDER_MODE_MANIP_ONLY,
	OBJ8_RENDER_MODE_MANIP_ONLY_ONE,
	OBJ8_RENDER_MODE_NONMANIP_ONLY_ONE
} obj8_render_mode_t;

typedef struct {
	unsigned	n_drs;
	avl_tree_t	tree;
	list_t		list;
	bool		complete;
	mutex_t		lock;
	float		*values;	/* protected by `lock` above */
	float		*trig_deltas;	// constant after init
} obj8_drset_t;

LIBRAIN_EXPORT obj8_t *obj8_parse(const char *filename, vect3_t pos_offset);
LIBRAIN_EXPORT void obj8_free(obj8_t *obj);
LIBRAIN_EXPORT bool obj8_needs_upload(const obj8_t *obj);

LIBRAIN_EXPORT void obj8_draw_group(obj8_t *obj, const char *groupname,
    GLuint prog, const mat4 mvp);

LIBRAIN_EXPORT void obj8_draw_group_by_cmdidx(obj8_t *obj, unsigned idx, GLuint prog,
    const mat4 pvm_in);

LIBRAIN_EXPORT void obj8_set_matrix(obj8_t *obj, mat4 matrix);

LIBRAIN_EXPORT obj8_render_mode_t obj8_get_render_mode(const obj8_t *obj);
LIBRAIN_EXPORT void obj8_set_render_mode(obj8_t *obj, obj8_render_mode_t mode);
LIBRAIN_EXPORT void obj8_set_render_mode2(obj8_t *obj, obj8_render_mode_t mode,
    int32_t arg);

LIBRAIN_EXPORT unsigned obj8_get_num_manips(const obj8_t *obj);
LIBRAIN_EXPORT const obj8_manip_t *obj8_get_manip(const obj8_t *obj,
    unsigned idx);

LIBRAIN_EXPORT unsigned obj8_get_num_cmd_t(const obj8_t *obj);
LIBRAIN_EXPORT const obj8_cmd_t *obj8_get_cmd_t(const obj8_t *obj, unsigned idx);
LIBRAIN_EXPORT unsigned obj8_get_cmd_drset_idx(const obj8_cmd_t *cmd);
LIBRAIN_EXPORT unsigned obj8_get_cmd_idx(const obj8_cmd_t *cmd);

LIBRAIN_EXPORT void obj8_set_light_level_override(obj8_t *obj, float value);
LIBRAIN_EXPORT float obj8_get_light_level_override(const obj8_t *obj);

LIBRAIN_EXPORT const char *obj8_get_filename(const obj8_t *obj);
LIBRAIN_EXPORT const char *obj8_get_tex_filename(const obj8_t *obj,
    bool wait_load);
LIBRAIN_EXPORT const char *obj8_get_norm_filename(const obj8_t *obj,
    bool wait_load);
LIBRAIN_EXPORT const char *obj8_get_lit_filename(const obj8_t *obj,
    bool wait_load);

LIBRAIN_EXPORT void obj8_set_drset_auto_update(obj8_t *obj, bool flag);
LIBRAIN_EXPORT bool obj8_get_drset_auto_update(const obj8_t *obj);
LIBRAIN_EXPORT obj8_drset_t *obj8_get_drset(const obj8_t *obj);

LIBRAIN_EXPORT obj8_drset_t *obj8_drset_new(void);
LIBRAIN_EXPORT void obj8_drset_destroy(obj8_drset_t *drset);
LIBRAIN_EXPORT void obj8_drset_mark_complete(obj8_drset_t *drset);

LIBRAIN_EXPORT unsigned obj8_drset_add(obj8_drset_t *drset, const char *name,
    float trig_delta);
LIBRAIN_EXPORT bool obj8_drset_update(obj8_drset_t *drset);
LIBRAIN_EXPORT const char *obj8_drset_get_dr_name(const obj8_drset_t *drset,
    unsigned idx);

LIBRAIN_EXPORT unsigned obj8_drset_get_num_drs(const obj8_drset_t *drset);

//CONCERN: Are these two needed?
LIBRAIN_EXPORT int obj8_drset_get_dr_offset(const obj8_drset_t *drset, unsigned idx);
LIBRAIN_EXPORT const char *obj8_manip_type_t_name(obj8_manip_type_t type_val);


LIBRAIN_EXPORT void
obj8_debug_cmd(const obj8_t *obj, const obj8_cmd_t *subcmd);

LIBRAIN_EXPORT unsigned 
obj8_get_manip_idx_from_cmd_tris(const obj8_cmd_t *cmd);

LIBRAIN_EXPORT void
obj8_set_cmd_tris_hover_detectable(const obj8_cmd_t *cmd, bool detectable);

LIBRAIN_EXPORT unsigned
obj8_nearest_tris_for_cmd(const obj8_t *obj, const obj8_cmd_t *cmd);

void obj8_draw_by_counter(obj8_t *obj, GLuint prog, unsigned int todraw, mat4 pvm_in);


LIBRAIN_EXPORT void obj8_set_manip_paint_offset(obj8_t *obj, unsigned paint_offset);

static inline float
obj8_drset_getf(const obj8_drset_t *drset, unsigned idx)
{
	ASSERT(drset != NULL);
	ASSERT(drset->complete);
	ASSERT3U(idx, <, drset->n_drs);
	mutex_enter((mutex_t *)&drset->lock);
	float value = drset->values[idx];
	mutex_exit((mutex_t *)&drset->lock);
	return (value);
}

LIBRAIN_EXPORT size_t obj8_drset_get_all(const obj8_drset_t *drset,
    float *out_values, size_t cap);

LIBRAIN_EXPORT dr_t *obj8_drset_get_dr(const obj8_drset_t *drset,
    unsigned idx);

#ifdef __cplusplus
}
#endif

#endif	/* _OBJ8_H_ */
