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

#version 120
#extension GL_EXT_gpu_shader4: require

uniform sampler2D	tex;
uniform sampler2D	temp_tex;
uniform vec2		my_tex_sz;
uniform vec2		tp;
uniform float		thrust;
uniform float		precip_intens;
uniform float		window_ice;

/*
 * Gold Noise ©2017-2018 dcerisano@standard3d.com.
 *  - based on the Golden Ratio, PI and Square Root of Two
 *  - fastest noise generator function
 *  - works with all chipsets (including low precision)
 */

const float PHI = 1.61803398874989484820459 * 00000.1;	/* Golden Ratio */
const float PI  = 3.14159265358979323846264 * 00000.1;	/* PI */
const float SQ2 = 1.41421356237309504880169 * 10000.0;	/* Square Root of Two */

const float max_depth = 3;
const float temp_scale_fact = 400.0;
const float water_liquid_temp = 273 + 2;	/* 5 degrees C */
const float water_frozen_temp = 273 - 2;	/* -3 degrees C */

float
gold_noise(vec2 coordinate, float seed)
{
	return (fract(sin(dot(coordinate * (seed + PHI), vec2(PHI, PI))) *
	    SQ2));
}

float
read_depth(vec2 pos)
{
	return (texture2D(tex, pos / textureSize2D(tex, 0)).r);
}

void
main()
{
	float depth_left, depth_right, depth_up, depth_down;
	float d_lr, d_ud;
	float temp = texture2D(temp_tex, gl_FragCoord.xy / my_tex_sz).r *
	    temp_scale_fact;
	vec2 thrust_v = (gl_FragCoord.xy - tp);
	vec2 ice_displace = vec2(0, 0);
	float window_ice_fact = sqrt(min(window_ice, 1));

	if (temp < water_liquid_temp) {
		float fact = min((temp - water_liquid_temp) /
		    (water_frozen_temp - water_liquid_temp), 1) * 0.1;
		ice_displace = vec2(
		    (gold_noise(gl_FragCoord.xy, 0.0) - 0.5) * fact,
		    (gold_noise(gl_FragCoord.xy, 1.0) - 0.5) * fact);
	}

	thrust_v /= length(thrust_v);
	thrust_v *= 20 * thrust + 1;

	depth_left = read_depth(gl_FragCoord.xy + vec2(-1, 0) * thrust_v);
	depth_right = read_depth(gl_FragCoord.xy + vec2(1, 0) * thrust_v);
	depth_up = read_depth(gl_FragCoord.xy + vec2(0, 1) * thrust_v);
	depth_down = read_depth(gl_FragCoord.xy + vec2(0, -1) * thrust_v);

	d_lr = ((atan(depth_left - depth_right) / (3.1415 / 2)) *
	    (1 + ice_displace.x)) + 0.5;
	d_lr = clamp(d_lr + window_ice_fact * ice_displace.x, 0, 1);

	d_ud = ((atan(depth_up - depth_down) / (3.1415 / 2)) *
	    (1 + ice_displace.y)) + 0.5;
	d_ud = clamp(d_ud + window_ice_fact * ice_displace.y, 0, 1);

	gl_FragColor = vec4(d_lr, d_ud, 0.0, 1.0);
}
