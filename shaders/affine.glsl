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

#ifndef	_AFFINE_GLSL_
#define	_AFFINE_GLSL_

mat3
mat3_translate(mat3 m, vec2 offset)
{
	mat3 offset_mat = mat3(
	    1.0, 0.0, 0.0,
	    0.0, 1.0, 0.0,
	    offset.x, offset.y, 1.0
	);
	return (offset_mat * m);
}

mat3
mat3_scale(mat3 m, vec2 scale)
{
	mat3 scale_mat = mat3(
	    scale.x, 0.0, 0.0,
	    0.0, scale.y, 0.0,
	    0.0, 0.0, 1.0
	);
	return (scale_mat * m);
}

mat3
mat3_rotate(mat3 m, float theta)
{
	float sin_theta = sin(theta);
	float cos_theta = cos(theta);
	mat3 rotate_mat = mat3(
	    cos_theta, sin_theta, 0.0,
	    -sin_theta, cos_theta, 0.0,
	    0.0, 0.0, 1.0
	);
	return (rotate_mat * m);
}

mat3
mat3_rotate_dir(mat3 m, vec2 dir)
{
	mat3 rotate_mat = mat3(
	    dir.y, -dir.x, 0.0,
	    dir.x, dir.y, 0.0,
	    0.0, 0.0, 1.0
	);
	return (rotate_mat * m);
}

#endif	/* _AFFINE_GLSL_ */
