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

#include "consts.glsl"
#include "droplets_data.h"
#include "util.glsl"
#include "noise.glsl"

#define	MAX_WIPERS	4

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
layout(location = 21) uniform float	wind_temp;

layout(location = 29) uniform int	num_wipers;
layout(location = 30) uniform vec2	wiper_pivot[MAX_WIPERS];
layout(location = 34) uniform float	wiper_radius_outer[MAX_WIPERS];
layout(location = 38) uniform float	wiper_radius_inner[MAX_WIPERS];
layout(location = 42) uniform float	wiper_pos_cur[MAX_WIPERS]; /* radians */
layout(location = 46) uniform float	wiper_pos_prev[MAX_WIPERS];

layout(location = 0) out vec4	color_out;

const float precip_fact = 0.1;
const float gravity_factor = 0.25;
const float precip_scale_fact = 0.0117;
const float temp_scale_fact = 400.0;
const float water_liquid_temp = C2KELVIN(1.5);
const float water_frozen_temp = C2KELVIN(-0.5);

float
gentle_random(float factor)
{
	return (sin((gl_FragCoord.x + gl_FragCoord.y) / factor ) *
	    cos((gl_FragCoord.x + gl_FragCoord.y) / (factor * 0.31415)));
}

/*
 * We calculate the distance of how far between the wiper's cur and prev
 * angles this pixel is located. A value of 1.0 means it's right on the
 * blade. A value of 0.0 means the pixel just dropped out of the smearing
 * and rain fadeout region.
 */
float
wiper_coeff(vec2 coord, int i)
{
	vec2 wiper2pos = coord - wiper_pivot[i];
	float dist = length(wiper2pos);
	float wiper2pos_angle;
	float f;

	if (dist > wiper_radius_outer[i] * 1.02 ||
	    dist < wiper_radius_inner[i] ||
	    wiper_pos_prev[i] == wiper_pos_cur[i])
		return (0.0);

	wiper2pos_angle = dir2hdg(wiper2pos);

	if (degrees(abs(wiper2pos_angle - wiper_pos_cur[i])) <
	    0.8 + 0.3 * gentle_random(25.0))
		return (2.0);

	f = iter_fract(wiper2pos_angle, wiper_pos_prev[i], wiper_pos_cur[i]);
	if (f < 0.0 || f > 1.0)
		return (0.0);
	return (f);
}

bool
droplet_gen_check(vec2 pos)
{
	float temp_fact = clamp(fx_lin(wind_temp, C2KELVIN(-20.0), 0.0,
	    C2KELVIN(0.0), 1.0), 0.0, 1.0);
	float prob = precip_fact * precip_intens * precip_scale_fact *
	    temp_fact * max(pow(min(1.0 - thrust, 1.0 - wind), 1.0), 0.35);

	return (gold_noise(pos, rand_seed) > (1.0 - prob));
}

float
read_depth(vec2 pos)
{
	return (texture(tex, pos / textureSize(tex, 0)).r);
}

void
main()
{
	float old_depth, prev_depth;
	float new_depth = 0.0;
	float depth = 0.0;
	vec2 tex_sz = textureSize(tex, 0);
	vec2 prev_pos;
	vec2 tp_dir, gp_dir, wp_dir, rand_dir;
	vec4 old_val;
	float r = 0.0, g = 0.0, b = 0.0, a = 0.0;
	float blowaway_fact;
	bool water_added = false;
	vec2 tex_coord = gl_FragCoord.xy / tex_sz;
	float temp = texture(temp_tex, tex_coord).r * temp_scale_fact;
	float temp_flow_coeff;
	float wind_dist_factor, thrust_dist_factor;
	float old_pos_temp;
	float wiper_coeff_total = 0.0;

	if (temp > water_liquid_temp) {
		temp_flow_coeff = 1.0;
	} else if (temp < water_frozen_temp) {
		temp_flow_coeff = 0.0075;
	} else {
		temp_flow_coeff = max((temp - water_frozen_temp) /
		    (water_liquid_temp - water_frozen_temp), 0.0075);
	}

	if (droplet_gen_check(gl_FragCoord.xy)) {
		new_depth = max_depth;
		water_added = true;
	} else if (droplet_gen_check(gl_FragCoord.xy + vec2(1.0, 0.0)) ||
	    droplet_gen_check(gl_FragCoord.xy + vec2(-1.0, 0.0)) ||
	    droplet_gen_check(gl_FragCoord.xy + vec2(0.0, 1.0)) ||
	    droplet_gen_check(gl_FragCoord.xy + vec2(0.0, -1.0))) {
		new_depth = max_depth / 3.0;
		water_added = true;
	}

	depth += new_depth;

	old_depth = read_depth(gl_FragCoord.xy) *
	    (0.39 + 0.05 * (1.0 - temp_flow_coeff) *
	    mix(0.01, 1.0, max(thrust, wind)));
	depth += old_depth;

	gp_dir = (gl_FragCoord.xy - gp);
	gp_dir = gp_dir / length(gp_dir);

	tp_dir = (gl_FragCoord.xy - tp);
	tp_dir = tp_dir / length(tp_dir);

	wp_dir = (gl_FragCoord.xy - wp);
	wp_dir = wp_dir / length(wp_dir);

	wind_dist_factor = pow(thrust, 2.5);
	thrust_dist_factor = pow(wind, 1.85);

	prev_pos = gl_FragCoord.xy -
	    ((gp_dir * (gravity_factor * gravity * pow(precip_intens, 2.0)) +
	    tp_dir * thrust_dist_factor + wp_dir * wind_dist_factor) *
	    tex_sz * d_t * temp_flow_coeff);
	old_pos_temp = texture(temp_tex, prev_pos).r * temp_scale_fact;
	if (temp < water_liquid_temp || old_pos_temp > water_frozen_temp)
		prev_depth = read_depth(prev_pos);
	else
		prev_depth = 0.0;

	blowaway_fact = mix(0.57, 0.601, temp_flow_coeff);

	depth += prev_depth * blowaway_fact;

	if (!water_added) {
		float old_water = min(old_depth + prev_depth, max_depth);
		depth = clamp(depth, 0.0, old_water -
		    max_depth * mix(0.005, 0.01, temp_flow_coeff));
	}
	depth = max(depth, min_depth);

	/*
	 * Figure out where along the wiper's area of smear we are located.
	 * Then take the strongest "wiping" effect and use it to flatten
	 * our water depth. This makes the water gradually appear behind
	 * the wiper as it travels across the windshield.
	 */
	for (int i = 0; i < num_wipers; i++) {
		float f = wiper_coeff(tex_coord, i);
		/*
		 * If the coefficient function returns greater than 1.0,
		 * that means we are right on the leading edge of the wiper.
		 * In that case, we want to simulate water piling up.
		 */
		if (f > 1.0) {
			depth *= 10.0;
			wiper_coeff_total = 0.0;
			break;
		}
		wiper_coeff_total = max(wiper_coeff_total, f);
	}
	depth *= 1.0 - clamp(1.1 * sqrt(wiper_coeff_total), 0.0, 1.0);

	color_out = vec4(clamp(depth, 0.0, max_depth), 0.0, 0.0, 1.0);
}
