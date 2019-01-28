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

#version 460
#extension GL_GOOGLE_include_directive: require

#if	COMPUTE_VARIANT
#define	MY_TEX_SZ	2048.0
#define	TEX_SZ		2048.0
#else
#define	MY_TEX_SZ	1024.0
#define	TEX_SZ		1024.0
#endif

#include "noise.glsl"
#include "util.glsl"

layout(location = 10) uniform sampler2D	tex;
layout(location = 11) uniform sampler2D	temp_tex;

layout(location = 12) uniform vec2	tp;
layout(location = 13) uniform float	thrust;
layout(location = 14) uniform float	precip_intens;
layout(location = 15) uniform float	window_ice;

layout(location = 0) out vec4	color_out;

const float max_depth = 3.0;
const float temp_scale_fact = 400.0;
const float water_liquid_temp = C2KELVIN(2);
const float water_frozen_temp = C2KELVIN(-2);

float
read_depth(vec2 pos)
{
	return (texture(tex, pos / TEX_SZ).r);
}

void
main()
{
	float depth_left, depth_right, depth_up, depth_down, depth_here;
	float d_lr, d_ud;
	float temp = texture(temp_tex, gl_FragCoord.xy / MY_TEX_SZ).r *
	    temp_scale_fact;
	vec2 ice_displace = vec2(0.0);
	float window_ice_fact = sqrt(min(window_ice, 1.0));

	if (temp < water_liquid_temp) {
		float fact = min((temp - water_liquid_temp) /
		    (water_frozen_temp - water_liquid_temp), 1);
		float depth = pow(read_depth(gl_FragCoord.xy) / max_depth, 0.3);
		fact *= depth;
		ice_displace = vec2(
		    (gold_noise(gl_FragCoord.xy, 0.0) - 0.5) * fact,
		    (gold_noise(gl_FragCoord.xy, 1.0) - 0.5) * fact);
	}

#if	COMPUTE_VARIANT
	depth_here = read_depth(gl_FragCoord.xy);
	depth_left = (read_depth(gl_FragCoord.xy + vec2(-1.0, 0.0)) * 0.5) +
	    (read_depth(gl_FragCoord.xy + vec2(-2.0, 0.0)) * 0.25) +
	    depth_here * 0.25;
	depth_right = (read_depth(gl_FragCoord.xy + vec2(1.0, 0.0)) * 0.5) +
	    (read_depth(gl_FragCoord.xy + vec2(2.0, 0.0)) * 0.25) +
	    depth_here * 0.25;
	depth_up = (read_depth(gl_FragCoord.xy + vec2(0.0, 1.0)) * 0.5) +
	    (read_depth(gl_FragCoord.xy + vec2(0.0, 2.0)) * 0.25) +
	    depth_here * 0.25;
	depth_down = (read_depth(gl_FragCoord.xy + vec2(0.0, -1.0)) * 0.5) +
	    (read_depth(gl_FragCoord.xy + vec2(0.0, -2.0)) * 0.25) +
	    depth_here * 0.25;
#else	/* !COMPUTE_VARIANT */
	vec2 thrust_v = (gl_FragCoord.xy - tp);
	thrust_v /= length(thrust_v);
	thrust_v *= 20 * thrust + 1;

	depth_left = read_depth(gl_FragCoord.xy + vec2(-1, 0) * thrust_v);
	depth_right = read_depth(gl_FragCoord.xy + vec2(1, 0) * thrust_v);
	depth_up = read_depth(gl_FragCoord.xy + vec2(0, 1) * thrust_v);
	depth_down = read_depth(gl_FragCoord.xy + vec2(0, -1) * thrust_v);
#endif	/* !COMPUTE_VARIANT */

	d_lr = ((atan(depth_left - depth_right) / (3.1415 / 2)) *
	    (1 + ice_displace.x)) + 0.5;
	d_lr = clamp(d_lr + window_ice_fact * ice_displace.x, 0, 1);

	d_ud = ((atan(depth_up - depth_down) / (3.1415 / 2)) *
	    (1 + ice_displace.y)) + 0.5;
	d_ud = clamp(d_ud + window_ice_fact * ice_displace.y, 0, 1);

	color_out = vec4(d_lr, d_ud, 0.0, 1.0);
}
