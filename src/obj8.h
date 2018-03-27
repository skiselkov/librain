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

#include <acfutils/geom.h>
#include <acfutils/list.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	unsigned	n_vtx;		/* number of vertices in geometry */
	GLfloat		*vtx_pos;	/* three GLfloats per vertex */
	GLfloat		*vtx_norm;	/* three GLfloats per vertex */
	GLfloat		*vtx_tex;	/* two GLfloats per vertex */
	GLuint		vtx_pos_buf;
	GLuint		vtx_norm_buf;
	GLuint		vtx_tex_buf;
	char		group_id[32];	/* Contents of X-GROUP-ID attribute */
	bool_t		double_sided;
	list_node_t	node;
} obj8_geom_t;

typedef struct {
	list_t		geom;
} obj8_t;

obj8_t *obj8_parse(const char *filename, vect3_t pos_offset);
void obj8_free(obj8_t *obj);
void obj8_draw_group(const obj8_t *obj, const char *groupname);

#ifdef __cplusplus
}
#endif

#endif	/* _OBJ8_H_ */
