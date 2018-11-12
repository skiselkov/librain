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
 * Copyright 2018 Saso Kiselkov. All rights reserved.
 */

#ifndef	_OBJ8_H_
#define	_OBJ8_H_

#include <stdio.h>

#include <GL/glew.h>
#include <cglm/cglm.h>

#include <acfutils/dr.h>
#include <acfutils/geom.h>
#include <acfutils/list.h>

#ifdef __cplusplus
extern "C" {
#endif

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
	bool_t			null_dr;
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

typedef struct {
	void		*vtx_table;
	GLuint		vtx_buf;
	unsigned	vtx_cap;
	void		*idx_table;
	GLuint		idx_buf;
	unsigned	idx_cap;
	mat4		matrix;
	obj8_cmd_t	*top;
} obj8_t;

obj8_t *obj8_parse(const char *filename, vect3_t pos_offset);
void obj8_free(obj8_t *obj);
void obj8_draw_group(obj8_t *obj, const char *groupname, GLuint prog, mat4 mvp);
void obj8_set_matrix(obj8_t *obj, mat4 matrix);

#ifdef __cplusplus
}
#endif

#endif	/* _OBJ8_H_ */
