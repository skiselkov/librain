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

#ifndef	_LIBRAIN_H_
#define	_LIBRAIN_H_

#include <acfutils/types.h>
#include "obj8.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	const obj8_t	*obj;
	const char	**group_ids;

	double		slant_factor;

	vect2_t		thrust_point;
	double		thrust_factor;
	double		max_thrust;

	vect2_t		gravity_point;
	double		gravity_factor;

	vect2_t		wind_point;
	double		wind_factor;
	double		wind_normal;
	double		max_tas;

	float		therm_inertia;
	float		heat_zones[16];		/* 4 x 4 matrix */
	float		heat_tgt_temps[4];

	float		cabin_temp;
	float		hot_air_src[4];
	float		hot_air_radius[2];
	float		hot_air_temp[2];
} librain_glass_t;

bool_t librain_init(const char *the_pluginpath, const librain_glass_t *glass,
    size_t num);
void librain_fini(void);

bool_t librain_reload_gl_progs(void);

void librain_notify_VR(bool_t entering_VR);
void librain_draw_prepare(bool_t force);
void librain_draw_z_depth(const obj8_t *obj, const char **z_depth_group_ids);
void librain_draw_exec(void);
void librain_draw_finish(void);

GLuint librain_get_screenshot_tex(void);

#ifdef __cplusplus
}
#endif

#endif	/* _LIBRAIN_H_ */
