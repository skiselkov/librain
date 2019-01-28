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

#version 460 core
#extension GL_GOOGLE_include_directive: require

#include "droplets_data.h"

layout(location = 0) in vec2	pos;
layout(location = 1) in vec2	ctr;
layout(location = 2) in float	radius;
layout(location = 3) in float	size;

layout(location = 0) out float	dist_ratio;

void
main()
{
	/*
	 * Pre-calculate the distance ratio for the frag shader here.
	 * This way we can use the GPU's interpolator.
	 */
	dist_ratio = 1.0 - radius / size;
	/*
	 * Droplet physics coordinates are [0,0] at the lower left corner
	 * and [1,1] at the upper right. Clip-space coordinates are [0,0]
	 * in the >>center<<, [1,1] on the upper right and [-1,-1] on the
	 * lower left.
	 */
	gl_Position = vec4((pos + ctr) * 2.0 - 1.0, 1.0, 1.0);
}
