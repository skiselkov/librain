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

LIBRAIN_EXPORT obj8_t *obj8_parse(const char *filename, vect3_t pos_offset);
LIBRAIN_EXPORT void obj8_free(obj8_t *obj);
LIBRAIN_EXPORT void obj8_draw_group(obj8_t *obj, const char *groupname,
    GLuint prog, const mat4 mvp);
LIBRAIN_EXPORT void obj8_set_matrix(obj8_t *obj, mat4 matrix);
LIBRAIN_EXPORT const char *obj8_get_filename(const obj8_t *obj);

#ifdef __cplusplus
}
#endif

#endif	/* _OBJ8_H_ */
