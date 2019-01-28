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

#include "consts.glsl"
#include "droplets_data.h"
#include "util.glsl"

layout(early_fragment_tests) in;

layout(location = 0) in float	quant_fract;
layout(location = 1) in vec2	centerpt;
layout(location = 2) in float	max_width;

layout(location = 0) out vec4	color_out;

void
main()
{
	float centerpt_dist = length(centerpt - gl_FragCoord.xy);
	float width_ratio = 1 - (centerpt_dist / max_width);
	color_out = vec4(max_depth, 0, 0, quant_fract * width_ratio);
}
