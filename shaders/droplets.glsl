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

#include "affine.glsl"
#include "droplets_data.h"
#include "droplets_designs.glsl"
#include "noise.glsl"
#include "util.glsl"

#define	MAX_DRAG_STATIC		7.5
#define	MIN_DRAG_STATIC		0.0
#define	COEFF_DRAG_DYN		1.0

#define	MAX_RANDOM_DROP_RATE	0.98
#define	MIN_RANDOM_DROP_RATE	0.999

#define	REGEN_DELAY	\
	(0.5 + (gl_GlobalInvocationID.x / float(DROPLET_WG_SIZE)))

#define	DROPLET			(droplets.d[gl_GlobalInvocationID.x])

#define	FAST_SPD_LIM		0.1	/* tex/sec */
#define	VERY_FAST_SPD_LIM	0.2	/* tex/sec */

layout(local_size_x = DROPLET_WG_SIZE) in;

layout(constant_id = DEPTH_TEX_SZ_CONSTANT_ID) const int DEPTH_TEX_SZ = 1024;

layout(location = 0, r8) uniform restrict image2D depth_tex;
layout(location = 1) uniform sampler2D ws_temp_tex;
layout(location = 2) uniform float cur_t;
layout(location = 3) uniform float d_t;
layout(location = 4) uniform vec2 gravity_point;
layout(location = 5) uniform float gravity_force;
layout(location = 6) uniform vec2 wind_point;
layout(location = 7) uniform float wind_force;
layout(location = 8) uniform vec2 thrust_point;
layout(location = 9) uniform float thrust_force;
layout(location = 10) uniform float precip_intens;
layout(location = 11) uniform float min_droplet_sz;
layout(location = 12) uniform float le_temp;	/* Kelvin */

layout(location = 30) uniform float rand_seed[NUM_RANDOM_SEEDS];

layout(binding = 0) buffer droplet_data {
	droplet_data_t	d[];
} droplets;

void
depth_store(ivec2 pos, float depth)
{
	vec4 pixel;

	pos = clamp(pos, ivec2(0), ivec2(DEPTH_TEX_SZ - 1));
	pixel = imageLoad(depth_tex, pos);
	imageStore(depth_tex, pos,
	    vec4(min(depth + pixel.x, 1.0), 0.0, 0.0, 0.0));
}

vec2
droplet_forces_integrate()
{
	vec2 droplet_pos = DROPLET.pos[0];
	/* gravity, thrust and wind */
	vec2 F_a, F_g, F_t, F_w;

	F_g = normalize(droplet_pos - gravity_point) * gravity_force;
	F_t = normalize(droplet_pos - thrust_point) * thrust_force;
	F_w = normalize(droplet_pos - wind_point) * wind_force;

	F_a = (F_g + F_t + F_w);

	return (F_a);
}

void
bump_droplet(float velocity)
{
	const float bump_sz_slow = 40.0;
	const float bump_sz_fast = 160.0;
	const float velocity_mul = 0.3;
	const float bump_check_rate = 1.0;
	const float bump_prob_min = 0.25;
	const float bump_prob_max = 0.75;
	float rand_val, scale, bump_prob, velocity_clamped;

	if (cur_t - DROPLET.bump_t < bump_check_rate)
		return;
	DROPLET.bump_t = cur_t;
	velocity_clamped = clamp(velocity, 0.0, 1.0);
	bump_prob = mix(bump_prob_min, bump_prob_max, velocity_clamped);
	if (gold_noise(DROPLET.pos[0], rand_seed[0]) < (1.0 - bump_prob))
		return;
	rand_val = gold_noise(DROPLET.pos[0], rand_seed[1]);
	scale = mix(bump_sz_slow, bump_sz_fast, velocity_clamped) / DEPTH_TEX_SZ;

	DROPLET.bump_sz = scale * (rand_val - 0.5);
	DROPLET.velocity *= velocity_mul;
}

void
droplet_move()
{
	/* acceleration force */
	vec2 F_a = droplet_forces_integrate();
	/* drag force - dynamic */
	vec2 F_d_d = -DROPLET.velocity * COEFF_DRAG_DYN;
	/* drag force - static */
	vec2 F_d_s;
	/* final force */
	vec2 F;
	float F_d_s_len, coeff_vel;
	float rel_quant = DROPLET.quant / MAX_DROPLET_SZ;
	vec2 new_pos;
	float velocity = length(DROPLET.velocity);
	float rand_val, prob;

	coeff_vel = clamp(velocity, 0.0, 0.01);

	F_d_s_len = mix(MAX_DRAG_STATIC, MIN_DRAG_STATIC, coeff_vel) /
	    max(DROPLET.quant, 1.0);
	FILTER_IN(DROPLET.F_d_s_len, F_d_s_len, d_t, 0.5);
	rand_val = gold_noise(DROPLET.pos[0], rand_seed[2]);
	prob = mix(MIN_RANDOM_DROP_RATE, MAX_RANDOM_DROP_RATE, precip_intens);
	if (rand_val > prob)
		DROPLET.F_d_s_len = 0.0;

	if (DROPLET.F_d_s_len < length(F_a))
		F_d_s = normalize(F_a) * (-DROPLET.F_d_s_len);
	else
		F_d_s = -F_a;

	F = F_a + F_d_d + F_d_s;
	DROPLET.velocity += F * d_t;
	bump_droplet(velocity);

	new_pos = DROPLET.pos[0] + DROPLET.velocity * d_t;

	if (DROPLET.bump_sz > 0.0) {
		vec2 velocity_norm = vec2_norm_right(DROPLET.velocity);
		float bump_sz = min(velocity * d_t, DROPLET.bump_sz);

		DROPLET.bump_sz -= bump_sz;
		new_pos += normalize(velocity_norm) * bump_sz;
	} else if (DROPLET.bump_sz < 0.0) {
		vec2 velocity_norm = vec2_norm_right(DROPLET.velocity);
		float bump_sz = max(-velocity * d_t, DROPLET.bump_sz);

		DROPLET.bump_sz -= bump_sz;
		new_pos += normalize(velocity_norm) * bump_sz;
	}

	if (DROPLET.streamer) {
		float lag = 0.1 +
		    (gl_LocalInvocationIndex / (15.0 * DROPLET_WG_SIZE));
		FILTER_IN(DROPLET.pos[7], DROPLET.pos[6], d_t, lag);
		FILTER_IN(DROPLET.pos[6], DROPLET.pos[5], d_t, lag);
		FILTER_IN(DROPLET.pos[5], DROPLET.pos[4], d_t, lag);
		FILTER_IN(DROPLET.pos[4], DROPLET.pos[3], d_t, lag);
		FILTER_IN(DROPLET.pos[3], DROPLET.pos[2], d_t, lag);
		FILTER_IN(DROPLET.pos[2], DROPLET.pos[1], d_t, lag);
		FILTER_IN(DROPLET.pos[1], DROPLET.pos[0], d_t, lag);
		DROPLET.pos[0] = new_pos;
	} else {
		DROPLET.pos[1] = DROPLET.pos[0];
		DROPLET.pos[0] = new_pos;
	}
}

void
droplet_init_velocity(vec2 droplet_pos)
{
	DROPLET.velocity = vec2(0.0);

	/* gravity, thrust and wind */
	vec2 F_a, F_g, F_t, F_w;

	F_g = normalize(droplet_pos - gravity_point) * gravity_force;
	F_t = normalize(droplet_pos - thrust_point) * thrust_force;
	F_w = normalize(droplet_pos - wind_point) * wind_force;
	F_a = (F_g + F_t + F_w);
	if (length(F_a) > 0.0)
		DROPLET.tail_angle = dir2hdg(F_a);
}

void
droplet_regen()
{
	vec2 coord, droplet_pos;
	float droplet_sz_ratio;

	if (cur_t - DROPLET.regen_t < REGEN_DELAY)
		return;
	DROPLET.regen_t = cur_t;

	coord = vec2(gl_GlobalInvocationID);
	if (gold_noise(coord, rand_seed[3]) > precip_intens)
		return;

	droplet_pos = vec2(gold_noise(coord, rand_seed[4]),
	    gold_noise(coord, rand_seed[5]));
	for (int i = 0; i < NUM_DROPLET_HISTORY; i++)
		DROPLET.pos[i] = droplet_pos;
	droplet_sz_ratio = gold_noise(coord, rand_seed[6]);
	DROPLET.quant = mix(min_droplet_sz, MAX_DROPLET_SZ, droplet_sz_ratio);
	DROPLET.bump_sz = 0.0;

	droplet_init_velocity(droplet_pos);

	DROPLET.streamer = ((gl_GlobalInvocationID.x & 3) == 0);
}

void
droplet_paint()
{
#define DROPLET_PAINT_I(_pixels) \
	for (int i = 0, n = _pixels.length(); i < n; i++) { \
		depth_store(img_pos + ivec2(_pixels[i].pos), \
		    _pixels[i].depth); \
	}
#define DROPLET_PAINT_TAIL(_pixels) \
	if (velocity > 0.0) { \
		float velocity_fract = clamp(velocity * 100.0, 0.0, 1.0); \
		float alpha = mix(0.0, 1.0, velocity_fract); \
		float angle = dir2hdg(DROPLET.pos[0] - DROPLET.pos[1]); \
		FILTER_IN(DROPLET.tail_angle, angle, d_t, 0.2); \
		mat3 m = mat3_rotate(mat3(1.0), -DROPLET.tail_angle); \
		for (int i = 0, n = _pixels.length(); i < n; i++) { \
			vec2 p = vec2(m * vec3(_pixels[i].pos, 1.0)); \
			depth_store(ivec2(img_pos + p), \
			    _pixels[i].depth * alpha); \
		} \
	}

	ivec2 img_pos;
	vec2 back_v, back_v_norm;
	float back_v_len;
	vec2 right, left;
	float velocity = length(DROPLET.velocity);
	bool very_fast_droplet = (velocity > VERY_FAST_SPD_LIM);
	bool fast_droplet = (velocity > FAST_SPD_LIM);

	img_pos = ivec2(DROPLET.pos[0] * DEPTH_TEX_SZ);
	if (DROPLET.quant < 0.05 * MAX_DROPLET_SZ) {
		DROPLET_PAINT_I(droplet_style1);
	} else if (DROPLET.quant < 0.1 * MAX_DROPLET_SZ || very_fast_droplet ||
	    DROPLET.streamer) {
		DROPLET_PAINT_I(droplet_style2);
	} else if (DROPLET.quant < 0.2 * MAX_DROPLET_SZ) {
		DROPLET_PAINT_I(droplet_style3);
		DROPLET_PAINT_TAIL(droplet_style3_tail);
	} else if (DROPLET.quant < 0.3 * MAX_DROPLET_SZ || fast_droplet) {
		DROPLET_PAINT_I(droplet_style4);
		DROPLET_PAINT_TAIL(droplet_style4_tail);
	} else if (DROPLET.quant < 0.4 * MAX_DROPLET_SZ) {
		DROPLET_PAINT_I(droplet_style5);
		DROPLET_PAINT_TAIL(droplet_style5_tail);
	} else {
		DROPLET_PAINT_I(droplet_style6);
		DROPLET_PAINT_TAIL(droplet_style6_tail);
	}

	if (!DROPLET.streamer)
		return;

	back_v = (DROPLET.pos[1] - DROPLET.pos[0]) * DEPTH_TEX_SZ;
	back_v_len = length(back_v);
	back_v_norm = normalize(back_v) * 0.5;
	right = vec2_norm_right(back_v_norm);
	left = -right;
	if (very_fast_droplet) {
		for (vec2 v = back_v_norm; length(v) < back_v_len;
		    v += back_v_norm) {
			depth_store(img_pos + ivec2(v), 2.0);
			depth_store(img_pos + ivec2(v + left), 1.0);
			depth_store(img_pos + ivec2(v + right), 2.0);
		}
	} else {
		for (vec2 v = back_v_norm; length(v) < back_v_len;
		    v += back_v_norm) {
			depth_store(img_pos + ivec2(v), 3.0);
			depth_store(img_pos + ivec2(v + left), 2.0);
			depth_store(img_pos + ivec2(v + 2.0 * left), 1.0);
			depth_store(img_pos + ivec2(v + 3.0 * left), 0.5);
			depth_store(img_pos + ivec2(v + right), 2.0);
			depth_store(img_pos + ivec2(v + 2.0 * right), 1.0);
			depth_store(img_pos + ivec2(v + 3.0 * right), 0.5);
		}
	}

	img_pos = ivec2(DROPLET.pos[1] * DEPTH_TEX_SZ);
	back_v = (DROPLET.pos[2] - DROPLET.pos[1]) * DEPTH_TEX_SZ;
	back_v_len = length(back_v);
	back_v_norm = normalize(back_v) * 0.5;
	right = vec2_norm_right(back_v_norm);
	left = -right;
	if (very_fast_droplet) {
		for (vec2 v = back_v_norm; length(v) < back_v_len;
		    v += back_v_norm) {
			depth_store(img_pos + ivec2(v), 1.0);
		}
	} else {
		for (vec2 v = back_v_norm; length(v) < back_v_len;
		    v += back_v_norm) {
			depth_store(img_pos + ivec2(v), 2.0);
			depth_store(img_pos + ivec2(v + left), 1.0);
			depth_store(img_pos + ivec2(v + 2.0 * left), 0.5);
			depth_store(img_pos + ivec2(v + right), 1.0);
			depth_store(img_pos + ivec2(v + 2.0 * right), 0.5);
		}
	}

#define	PAINT_TRAIL_1(i1, i2, depth) \
	img_pos = ivec2(DROPLET.pos[i1] * DEPTH_TEX_SZ); \
	back_v = (DROPLET.pos[i2] - DROPLET.pos[i1]) * DEPTH_TEX_SZ; \
	back_v_len = length(back_v); \
	back_v_norm = normalize(back_v) * 0.5; \
	for (vec2 v = back_v_norm; length(v) < back_v_len; v += back_v_norm) { \
		depth_store(img_pos + ivec2(v), depth); \
		depth_store(img_pos + ivec2(v + left), depth / 2.0); \
		depth_store(img_pos + ivec2(v + right), depth / 2.0); \
	}
	PAINT_TRAIL_1(2, 3, 1.0);
	PAINT_TRAIL_1(3, 4, 0.5);
	PAINT_TRAIL_1(4, 5, 0.3);
	PAINT_TRAIL_1(5, 6, 0.2);
	PAINT_TRAIL_1(6, 7, 0.1);
}

bool
droplet_should_exist()
{
	if (DROPLET.quant <= 0.0)
		return (false);
	if (DROPLET.streamer) {
		return (DROPLET.pos[NUM_DROPLET_HISTORY - 1].x > 0.0 &&
		    DROPLET.pos[NUM_DROPLET_HISTORY - 1].x < 1.0 &&
		    DROPLET.pos[NUM_DROPLET_HISTORY - 1].y > 0.0 &&
		    DROPLET.pos[NUM_DROPLET_HISTORY - 1].y < 1.0);
	} else {
		return (DROPLET.pos[0].x > 0.0 && DROPLET.pos[0].x < 1.0 &&
		    DROPLET.pos[0].y > 0.0 && DROPLET.pos[0].y < 1.0);
	}
}

void
main(void)
{
	if (droplet_should_exist()) {
		droplets.d[gl_GlobalInvocationID.x].quant -= d_t;
		droplet_move();
		droplet_paint();
	} else {
		/* Regen droplet at new location */
		droplet_regen();
	}
}
