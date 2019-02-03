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

#version 460 core
#extension GL_GOOGLE_include_directive: require

#include "noise.glsl"

layout(location = 0) uniform mat4	pvm;
layout(location = 1) uniform float	growth_mult;

/* from previous invocation of frag shader */
layout(location = 10) uniform sampler2D	depth;

layout(location = 0) in vec3		vtx_pos;
layout(location = 1) in vec3		vtx_norm;
layout(location = 2) in vec2		vtx_tex0;

layout(location = 0) out vec3		tex_norm;
layout(location = 1) out vec2		tex_coord;

#define	REF_DEPTH	1.5
#define	DEPTH_COEFF	(0.0001)
#define	MIN_DEPTH	0.001

#define	POW3(x)		((x) * (x) * (x))

void
main()
{
	float depth_val = texture(depth, vtx_tex0).r;
	float depth_rat = depth_val / REF_DEPTH;
	float rand_val = gold_noise(vtx_tex0 * textureSize(depth, 0), 1.0);
	vec3 rand_pos = max(DEPTH_COEFF * growth_mult * POW3(depth_rat),
	    MIN_DEPTH) * vtx_norm;

	tex_norm = vtx_norm;
	tex_coord = vtx_tex0;
	gl_Position = pvm * vec4(vtx_pos + rand_pos, 1.0);
}
