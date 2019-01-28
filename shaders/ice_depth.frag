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

#define	SRC_TYPE_POINT	0
#define	SRC_TYPE_LINE	1
#define	POW2(x)		((x) * (x))
#define	POW4(x)		((x) * (x) * (x) * (x))
#define	POW6(x)		((x) * (x) * (x) * (x) * (x) * (x))
#define	LINE_EDGE_X1	0.1
#define	LINE_EDGE_X2	0.9
#define	MAX_ICE		20

/* render pass inputs */
layout(location = 10) uniform sampler2D	prev;
layout(location = 11) uniform float	ice;
layout(location = 12) uniform float	d_ice;
layout(location = 13) uniform float	d_t;
layout(location = 14) uniform float	seed;

/* from vertex shader */
layout(location = 0) in vec3		tex_norm;
layout(location = 1) in vec2		tex_coord;

/* frag shader output */
layout(location = 0) out vec4		color_out;

float
origin_distance()
{
#if	SRC_TYPE == SRC_TYPE_POINT
	return (2 * length(vec2(tex_coord.x - 0.5, tex_coord.y - 0.5)));
#else	/* SRC_TYPE == SRC_TYPE_LINE */
	if (tex_coord.x < LINE_EDGE_X1) {
		return (length(vec2((LINE_EDGE_X1 - tex_coord.x) *
		    (1.0 / LINE_EDGE_X1), tex_coord.y - 0.5)));
	}
	if (tex_coord.x > LINE_EDGE_X2) {
		return (length(vec2((tex_coord.x - LINE_EDGE_X2) *
		    (1.0 / LINE_EDGE_X1), tex_coord.y - 0.5)));
	}
	return (abs(tex_coord.y - 0.5));
#endif	/* SRC_TYPE == SRC_TYPE_LINE */
}

#if	ADD_ICE

vec4
add_ice(float prev_depth)
{
	float rand_val = gold_noise(gl_FragCoord.xy, seed);
	float dist = clamp(1 - origin_distance(), 0, 1);
	float extra_ice = 0;

	if (rand_val < d_t * POW6(dist))
		extra_ice = max(100 * d_ice, 0.0075);
	return (vec4(min(prev_depth + extra_ice, MAX_ICE), 0, 0, 1));
}

#else	/* !ADD_ICE */

vec4
remove_ice(float prev_depth)
{
#if	DEICE
	vec2 coord = round(vec2(
	    gl_FragCoord.x / 10 + gl_FragCoord.y / (1 + (seed * 5.3)),
	    gl_FragCoord.y / 10 - gl_FragCoord.x / (1 + (seed * 4.7))));
	float rand_val = gold_noise(coord, seed);

	if (rand_val < 3 * d_t)
		return (vec4(0, 0, 0, 1));
	return (vec4(prev_depth, 0, 0, 1));
#else	/* !DEICE */
	float rand_val = gold_noise(gl_FragCoord.xy, seed);
	float dist = clamp(1 - origin_distance(), 0, 1);
	float extra_ice = 0;

	if (rand_val < d_t * POW6(dist))
		extra_ice = min(100 * d_ice, -0.001);
	return (vec4(max(prev_depth + extra_ice, 0), 0, 0, 1));
#endif	/* !DEICE */
}

#endif	/* !ADD_ICE */

void
main()
{
	float prev_depth = texture(prev, tex_coord).r;
#if	ADD_ICE
	color_out = add_ice(prev_depth);
#else
	color_out = remove_ice(prev_depth);
#endif
}
