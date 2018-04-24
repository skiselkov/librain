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

layout(location = 10) uniform sampler2D	tex;
layout(location = 11) uniform sampler2D	temp_tex;

layout(location = 12) uniform float	rand_seed;
layout(location = 13) uniform float	precip_intens;
layout(location = 14) uniform float	d_t;
layout(location = 15) uniform vec2	tp;	/* thrust origin point */
layout(location = 16) uniform vec2	gp;	/* gravity origin point */
layout(location = 17) uniform vec2	wp;	/* wind origin point */
layout(location = 18) uniform float	thrust;
layout(location = 19) uniform float	gravity;
layout(location = 20) uniform float	wind;

layout(location = 0) out vec4	color_out;

/*
 * Gold Noise ©2017-2018 dcerisano@standard3d.com 
 *  - based on the Golden Ratio, PI and Square Root of Two
 *  - fastest noise generator function
 *  - works with all chipsets (including low precision)
 */

const float PHI = 1.61803398874989484820459 * 00000.1;	/* Golden Ratio */
const float PI  = 3.14159265358979323846264 * 00000.1;	/* PI */
const float SQ2 = 1.41421356237309504880169 * 10000.0;	/* Square Root of Two */
const float max_depth = 3.0;
const float precip_fact = 0.1;
const float gravity_factor = 0.05;
const float precip_scale_fact = 0.02;
const float temp_scale_fact = 400.0;
const float water_liquid_temp = 273 + 2;	/* 5 degrees C */
const float water_frozen_temp = 273 - 2;	/* -3 degrees C */

float
gold_noise(vec2 coordinate, float seed)
{
	return (fract(sin(dot(coordinate * (seed + PHI), vec2(PHI, PI))) *
	    SQ2));
}

bool
droplet_gen_check(vec2 pos, float temp_flow_coeff)
{
	return (gold_noise(pos, rand_seed) >
	    (1 - (precip_fact * precip_intens * precip_scale_fact *
	    max(pow(min(1 - thrust, 1 - wind), 1), 0.35))));
}

float
read_depth(vec2 pos)
{
	return (texture(tex, pos / textureSize(tex, 0)).r);
}

void
main()
{
	float old_depth, depth, prev_depth, new_depth;
	vec2 tex_sz = textureSize(tex, 0);
	vec2 prev_pos;
	vec2 tp_dir, gp_dir, wp_dir, rand_dir;
	vec4 old_val;
	float r = 0, g = 0, b = 0, a = 0;
	float blowaway_fact;
	bool water_added;
	float temp = texture(temp_tex, gl_FragCoord.xy / tex_sz).r *
	    temp_scale_fact;
	float temp_flow_coeff;

	if (temp > water_liquid_temp) {
		temp_flow_coeff = 1;
	} else if (temp < water_frozen_temp) {
		temp_flow_coeff = 0;
	} else {
		temp_flow_coeff = (temp - water_frozen_temp) /
		    (water_liquid_temp - water_frozen_temp);
	}

	if (droplet_gen_check(gl_FragCoord.xy, temp_flow_coeff)) {
		new_depth = max_depth;
		water_added = true;
	} else if (droplet_gen_check(gl_FragCoord.xy + vec2(1, 0),
	    temp_flow_coeff) ||
	    droplet_gen_check(gl_FragCoord.xy + vec2(-1, 0), temp_flow_coeff) ||
	    droplet_gen_check(gl_FragCoord.xy + vec2(0, 1), temp_flow_coeff) ||
	    droplet_gen_check(gl_FragCoord.xy + vec2(0, -1),
	    temp_flow_coeff)) {
		new_depth = max_depth / 3;
		water_added = true;
	} else {
		new_depth = 0.0;
		water_added = false;
	}

	depth += new_depth;

	old_depth = read_depth(gl_FragCoord.xy) *
	    (0.39 + 0.05 * (1 - temp_flow_coeff) *
	    mix(0.01, 1, max(thrust, wind)));
	depth += old_depth;

	gp_dir = (gl_FragCoord.xy - gp);
	gp_dir = gp_dir / length(gp_dir);

	tp_dir = (gl_FragCoord.xy - tp);
	tp_dir = tp_dir / length(tp_dir);

	wp_dir = (gl_FragCoord.xy - wp);
	wp_dir = wp_dir / length(wp_dir);

	prev_pos = gl_FragCoord.xy -
	    ((gp_dir * (gravity_factor * gravity * pow(precip_intens, 2)) +
	    tp_dir * pow(thrust, 1.25) + wp_dir * pow(wind, 1.5)) *
	    tex_sz * d_t * temp_flow_coeff);
	prev_depth = read_depth(prev_pos);

	blowaway_fact = 0.601;

	depth += prev_depth * blowaway_fact;

	if (!water_added) {
		float old_water = min(old_depth + prev_depth, max_depth);
		depth = clamp(depth, 0.0, old_water -
		    max_depth * mix(0.001, 0.01, temp_flow_coeff));
	}

	color_out = vec4(clamp(depth, 0.0, max_depth), 0, 0, 1);
}
