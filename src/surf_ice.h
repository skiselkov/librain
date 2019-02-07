/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license in the file COPYING
 * or http://www.opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file COPYING.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2018 Saso Kiselkov. All rights reserved.
 */

#ifndef	_LIBRAIN_SURF_ICE_H_
#define	_LIBRAIN_SURF_ICE_H_

#include <acfutils/types.h>

#include "obj8.h"

#ifdef	__cplusplus
extern "C" {
#endif

typedef enum {
	SURF_ICE_SRC_POINT,
	SURF_ICE_SRC_LINE
} surf_ice_src_type_t;

typedef enum {
	SURF_DEICE_NONE,
	SURF_DEICE_MELT,
	SURF_DEICE_MECH
} surf_deice_type_t;

typedef struct surf_ice_impl_s surf_ice_impl_t;

typedef struct {
	const char		*name;	/* optional */
	unsigned		w;	/* pixels */
	unsigned		h;	/* pixels */
	surf_ice_src_type_t	src;
	surf_deice_type_t	deice;
	float			rand_seed;
	float			growth_mult;
	surf_ice_impl_t		*priv;
} surf_ice_t;

bool_t surf_ice_glob_init(const char *shaderpath);
void surf_ice_glob_fini(void);
bool_t surf_ice_reload_gl_progs(void);

void surf_ice_init(surf_ice_t *surf, obj8_t *obj, const char *group_id);
void surf_ice_fini(surf_ice_t *surf);
void surf_ice_render(surf_ice_t *surf, double ice, bool_t deice_on,
    double blur_radius, bool_t visible);
void surf_ice_clear(surf_ice_t *surf);

bool_t surf_ice_render_pass_needed(void);
void surf_ice_render_pass_begin(void);
void surf_ice_render_pass_done(void);

#ifdef	__cplusplus
}
#endif

#endif	/* _LIBRAIN_SURF_ICE_H_ */
