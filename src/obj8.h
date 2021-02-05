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
 * Copyright 2021 Saso Kiselkov. All rights reserved.
 */

#ifndef	_OBJ8_H_
#define	_OBJ8_H_

#include <stdio.h>

#include <XPLMUtilities.h>

#include <acfutils/dr.h>
#include <acfutils/geom.h>
#include <acfutils/glew.h>
#include <acfutils/list.h>

#include <cglm/cglm.h>

#include "librain_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct obj8_s obj8_t;

typedef enum {
	OBJ8_MANIP_AXIS_KNOB,
	OBJ8_MANIP_COMMAND,
	OBJ8_MANIP_COMMAND_AXIS,
	OBJ8_MANIP_COMMAND_KNOB,
	OBJ8_MANIP_COMMAND_SWITCH_LR,
	OBJ8_MANIP_COMMAND_SWITCH_UD,
	OBJ8_MANIP_DRAG_AXIS,
	OBJ8_MANIP_DRAG_ROTATE,
	OBJ8_MANIP_DRAG_XY
} obj8_manip_type_t;

typedef struct {
	obj8_manip_type_t	type;
	union {
		struct {
			float		min, max;
			float		d_click, d_hold;
			dr_t		dr;
		} manip_axis_knob;
		XPLMCommandRef		cmd;
		struct {
			float		dx, dy, dz;
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
		struct {
			float		dx, dy, dz;
			float		v1, v2;
		} drag_axis;
		struct {
			float		x, y, z;
			float		dx, dy, dz;
			float		angle1, angle2;
			float		lift;
			float		v1min, v1max;
			float		v2min, v2max;
			dr_t		dr1, dr2;
		} drag_rotate;
		struct {
			float		dx, dy;
			float		v1min, v1max;
			float		v2min, v2max;
			dr_t		dr1, dr2;
		} drag_xy;
	};
} obj8_manip_t;

LIBRAIN_EXPORT obj8_t *obj8_parse(const char *filename, vect3_t pos_offset);
LIBRAIN_EXPORT void obj8_free(obj8_t *obj);
LIBRAIN_EXPORT void obj8_draw_group(obj8_t *obj, const char *groupname,
    GLuint prog, const mat4 mvp);
LIBRAIN_EXPORT void obj8_set_matrix(obj8_t *obj, mat4 matrix);

LIBRAIN_EXPORT const char *obj8_get_filename(const obj8_t *obj);
LIBRAIN_EXPORT const char *obj8_get_tex_filename(const obj8_t *obj,
    bool wait_load);
LIBRAIN_EXPORT const char *obj8_get_norm_filename(const obj8_t *obj,
    bool wait_load);
LIBRAIN_EXPORT const char *obj8_get_lit_filename(const obj8_t *obj,
    bool wait_load);

#ifdef __cplusplus
}
#endif

#endif	/* _OBJ8_H_ */
