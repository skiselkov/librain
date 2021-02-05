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

#ifndef	_LIBRAIN_OBJMGR_H_
#define	_LIBRAIN_OBJMGR_H_

#include <stdbool.h>

#include "obj8.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct objmgr_s objmgr_t;
typedef struct objmgr_obj_s objmgr_obj_t;

objmgr_t *objmgr_new(void);
void objmgr_destroy(objmgr_t *mgr);

objmgr_obj_t *objmgr_add_obj(objmgr_t *mgr, const char *filename,
    bool lazy_load_textures);
void objmgr_remove_obj(objmgr_t *mgr, objmgr_obj_t *obj);

obj8_t *objmgr_get_obj8(const objmgr_obj_t *obj);
void objmgr_bind_textures(objmgr_t *mgr, objmgr_obj_t *obj,
    unsigned start_idx, int *tex_idx, int *norm_idx, int *lit_idx);

unsigned objmgr_get_num_objs(const objmgr_t *mgr);
unsigned objmgr_get_num_texs(const objmgr_t *mgr);

#ifdef __cplusplus
}
#endif

#endif	/* _LIBRAIN_OBJMGR_H_ */
