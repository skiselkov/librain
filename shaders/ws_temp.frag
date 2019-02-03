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

/*
 * N.B. all temps are in Kelvin!
 */

#define	CABIN_TEMP_INERTIA_FACTOR	2

layout(location = 10) uniform sampler2D		src;
layout(location = 11) uniform sampler2D		depth;

layout(location = 12) uniform float		rand_seed;
layout(location = 13) uniform float		le_temp;
layout(location = 14) uniform float		cabin_temp;
layout(location = 15) uniform float		wind_fact;
layout(location = 16) uniform float		d_t;
layout(location = 17) uniform float		inertia_in;
layout(location = 18) uniform float		precip_intens;

layout(location = 20) uniform vec4		heat_zones[4];
layout(location = 30) uniform float		heat_tgt_temps[4];
layout(location = 40) uniform vec2		hot_air_src[2];
layout(location = 50) uniform float		hot_air_radius[2];
layout(location = 60) uniform float		hot_air_temp[2];

layout(location = 0) out vec4	color_out;

const float max_depth = 3.0;
const float rand_temp_scale = 5.0;
const float temp_scale_fact = 400.0;
const vec2 null_vec2 = vec2(-1000000.0);

float
filter_in(float old_val, float new_val, float rate)
{
	float delta = new_val - old_val;
	float abs_delta = abs(delta);
	float inc_val = (delta * d_t) / rate;
	return (old_val + clamp(inc_val, -abs_delta, abs_delta));
}

void
main()
{
	vec2 my_size = textureSize(src, 0);
	float glass_temp = texture(src, gl_FragCoord.xy / my_size).r *
	    temp_scale_fact;
	float depth = texture(depth, gl_FragCoord.xy / textureSize(depth, 0)).r;
	float rand_temp = 4 * (gold_noise(gl_FragCoord.xy, rand_seed) - 0.5);
	float inertia = inertia_in * (1 + depth / max_depth);

	/* Protect from runaway values */
	if (glass_temp < 200 || glass_temp > temp_scale_fact)
		glass_temp = le_temp;

	glass_temp = filter_in(glass_temp, le_temp,
	    mix(inertia / 4, inertia / 40, min(wind_fact, 1)));
	glass_temp = filter_in(glass_temp, cabin_temp,
	    inertia * CABIN_TEMP_INERTIA_FACTOR);

	/*
	 * Hot air blowing on the windshield?
	 */
	for (int i = 0; i < 2; i++) {
		float hot_air_dist;
		float radius;

		if (hot_air_radius[i] <= 0)
			continue;

		hot_air_dist = length(gl_FragCoord.xy -
		    hot_air_src[i] * my_size);
		radius = hot_air_radius[i] * my_size.x;
		glass_temp = filter_in(glass_temp, hot_air_temp[i],
		    1.5 * max(inertia * (hot_air_dist / radius), 1));
	}

	glass_temp = filter_in(glass_temp, glass_temp + rand_temp, 0.5);

	for (int i = 0; i < 4; i++) {
		float left, right, bottom, top, inertia_out = 100000;

		if (heat_zones[i].z == 0 || heat_zones[i].w == 0 ||
		    heat_tgt_temps[i] == 0)
			continue;

		left = heat_zones[i].x * my_size.x;
		right = heat_zones[i].y * my_size.x;
		bottom = heat_zones[i].z * my_size.y;
		top = heat_zones[i].w * my_size.y;

		if (left <= gl_FragCoord.x && right >= gl_FragCoord.x &&
		    bottom <= gl_FragCoord.y && top >= gl_FragCoord.y) {
			inertia_out = inertia_in;
		} else if (gl_FragCoord.x < left &&
		    gl_FragCoord.y >= bottom && gl_FragCoord.y <= top) {
			inertia_out = max(inertia_in * left - gl_FragCoord.x,
			    inertia_in);
		} else if (gl_FragCoord.x > right &&
		    gl_FragCoord.y >= bottom && gl_FragCoord.y <= top) {
			inertia_out = max(inertia_in * gl_FragCoord.x - right,
			    inertia_in);
		}

		glass_temp = filter_in(glass_temp, heat_tgt_temps[i],
		    inertia_out);
	}

	color_out = vec4(glass_temp / temp_scale_fact, 0, 0, 1.0);
}
