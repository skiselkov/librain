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

/* render pass inputs */
layout(location = 10) uniform sampler2D	tex;
layout(location = 11) uniform float	rand_seed;
layout(location = 12) uniform float	blur_radius;

/* from vertex shader */
layout(location = 0) in vec3		tex_norm;
layout(location = 1) in vec2		tex_coord;

/* frag shader output */
layout(location = 0) out vec4		color_out;

vec2
vec_rot(vec2 v, float a)
{
	float sin_a = sin(-a), cos_a = cos(-a);
        return (vec2(v.x * cos_a - v.y * sin_a, v.x * sin_a + v.y * cos_a));
}

vec2
vec_norm(vec2 v)
{
	return (vec2(v.y, -v.x));
}

void
main()
{
	vec2 c2p = tex_coord - vec2(0.5);
	float l = max(length(c2p), 0.05);
	float radius = 4;
	float rand_val =
	    (2 * blur_radius * gold_noise(gl_FragCoord.xy, rand_seed)) / l;
	vec2 v[5];

	v[0] = vec2(0.5) + vec_rot(c2p, radians(-radius - rand_val));
	v[1] = vec2(0.5) + vec_rot(c2p, radians((-radius - rand_val) / 2));
	v[2] = vec2(0.5) + vec_rot(c2p, radians(rand_val));
	v[3] = vec2(0.5) + vec_rot(c2p, radians((radius + rand_val) / 2));
	v[4] = vec2(0.5) + vec_rot(c2p, radians(radius + rand_val));

	color_out += texture(tex, v[0]) * 0.1 +
	    texture(tex, v[1]) * 0.2 +
	    texture(tex, v[2]) * 0.4 +
	    texture(tex, v[3]) * 0.2 +
	    texture(tex, v[4]) * 0.1;
}
