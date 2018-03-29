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

/*
 * N.B. all temps are in Kelvin!
 */

uniform	sampler2D	src;
uniform	sampler2D	depth;
uniform float		rand_seed;
uniform float		le_temp;
uniform float		cabin_temp;
uniform float		wind_fact;
uniform float		d_t;
uniform float		inertia_in;
uniform vec4		heat_zones[4];
uniform float		heat_tgt_temps[4];
uniform float		precip_intens;

uniform vec2		hot_air_src[2];
uniform float		hot_air_radius[2];
uniform float		hot_air_temp[2];


/*
 * Gold Noise ©2017-2018 dcerisano@standard3d.com 
 *  - based on the Golden Ratio, PI and Square Root of Two
 *  - fastest noise generator function
 *  - works with all chipsets (including low precision)
 */
precision lowp float;

const float PHI = 1.61803398874989484820459 * 00000.1;	/* Golden Ratio */
const float PI  = 3.14159265358979323846264 * 00000.1;	/* PI */
const float SQ2 = 1.41421356237309504880169 * 10000.0;	/* Square Root of Two */
const float max_depth = 3.0;
const float rand_temp_scale = 5.0;
const float temp_scale_fact = 400.0;

float
gold_noise(vec2 coordinate, float seed)
{
	return (fract(sin(dot(coordinate * (seed + PHI), vec2(PHI, PI))) *
	    SQ2));
}

float
filter_in(float old_val, float new_val, float rate)
{
	float delta = new_val - old_val;
	float abs_delta = abs(delta);
	float inc_val = (delta * d_t) / rate;
	return (old_val + clamp(inc_val, -abs_delta, abs_delta));
}

vec2
vec2vec_isect(vec2 a, vec2 oa, vec2 b, vec2 ob, bool confined)
{
	vec2 p1, p2, p3, p4, r;
	float ca, cb, det;

	if (a == 0 || b == 0)
		return (vec2(0, 0));

	if (oa == ob)
		return (oa);

	p1 = oa;
	p2 = oa + a;
	p3 = ob;
	p4 = ob + b;

	det = (p1.x - p2.x) * (p3.y - p4.y) - (p1.y - p2.y) * (p3.x - p4.x);
	ca = p1.x * p2.y - p1.y * p2.x;
	cb = p3.x * p4.y - p3.y * p4.x;
	r.x = (ca * (p3.x - p4.x) - cb * (p1.x - p2.x)) / det;
	r.y = (ca * (p3.y - p4.y) - cb * (p1.y - p2.y)) / det;

	if (confined) {
		if (r.x < min(p1.x, p2.x) || r.x > max(p1.x, p2.x) ||
		    r.x < min(p3.x, p4.x) || r.x > max(p3.x, p4.x) ||
		    r.y < min(p1.y, p2.y) || r.y > max(p1.y, p2.y) ||
		    r.y < min(p3.y, p4.y) || r.y > max(p3.y, p4.y))
			return (vec2(-1, -1));
	}

	return (r);
}

void
main()
{
	vec2 my_size = textureSize2D(src, 0);
	float glass_temp = texture2D(src, gl_FragCoord.xy / my_size).r *
	    temp_scale_fact;
	float depth = texture2D(depth, gl_FragCoord.xy /
	    textureSize2D(depth, 0)).r;
	float rand_temp = 4 * (gold_noise(gl_FragCoord.xy, rand_seed) - 0.5);
	float inertia = inertia_in * (1 + depth);

	/* Protect from runaway values */
	glass_temp = clamp(glass_temp, 200, temp_scale_fact);

	glass_temp = filter_in(glass_temp, le_temp,
	    mix(inertia, inertia / 10, min(wind_fact + precip_intens, 1)));
	glass_temp = filter_in(glass_temp, cabin_temp, inertia * 10);

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
		    inertia + max(2 * inertia * (hot_air_dist / radius), 1));
	}

	glass_temp = filter_in(glass_temp, glass_temp + rand_temp, 0.5);

	for (int i = 0; i < 4; i++) {
		if (heat_zones[i].x * my_size.x <= gl_FragCoord.x &&
		    heat_zones[i].y * my_size.x >= gl_FragCoord.x &&
		    heat_zones[i].z * my_size.y <= gl_FragCoord.y &&
		    heat_zones[i].w * my_size.y >= gl_FragCoord.y) {
			glass_temp = filter_in(glass_temp, heat_tgt_temps[i],
			    inertia);
		} else if (heat_zones[i].z > 0) {
			vec2 ctr = vec2((heat_zones[i].x + heat_zones[i].y) / 2,
			    (heat_zones[i].z + heat_zones[i].w) / 2) * my_size;
			vec2 v = gl_FragCoord.xy - ctr;
			vec2 isect;
			vec2 p1, p2, p3, p4;
			float d;

			p1 = vec2(heat_zones[i].x, heat_zones[i].z) * my_size;
			p2 = vec2(heat_zones[i].x, heat_zones[i].w) * my_size;
			p3 = vec2(heat_zones[i].y, heat_zones[i].w) * my_size;
			p4 = vec2(heat_zones[i].y, heat_zones[i].z) * my_size;

			if ((isect = vec2vec_isect(v, ctr, p2 - p1, p1,
			    true)) != -1) {
			} else if ((isect = vec2vec_isect(v, ctr, p3 - p2, p2,
			    true)) != -1) {
			} else if ((isect = vec2vec_isect(v, ctr, p4 - p3, p3,
			    true)) != -1) {
			} else {
				isect = vec2vec_isect(v, ctr, p1 - p4, p4, true);
			}

			d = length(gl_FragCoord.xy - isect);

//			if (d > 10 && d < 100) {
//				glass_temp = filter_in(glass_temp,
//				    heat_tgt_temps[i], inertia * (d / 10.0));
//			}
		}
	}

	gl_FragColor = vec4(glass_temp / temp_scale_fact, 0, 0, 1.0);
}
