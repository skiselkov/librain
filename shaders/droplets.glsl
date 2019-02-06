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
#include "consts.glsl"
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

#define	DROPLET_ID	(gl_GlobalInvocationID.x)
#define	DROPLET		(droplets.d[DROPLET_ID])
#define	VERTEX(_idx)	\
	(vertices.v[DROPLET_ID * VTX_PER_DROPLET + (_idx)])
#define	TAIL(_idx)	\
	(tails.t[DROPLET_ID * NUM_DROPLET_HISTORY + (_idx)])

#define	FAST_SPD_LIM		0.1	/* tex/sec */
#define	VERY_FAST_SPD_LIM	0.2	/* tex/sec */

layout(local_size_x = DROPLET_WG_SIZE) in;

layout(constant_id = DEPTH_TEX_SZ_CONSTANT_ID) const int DEPTH_TEX_SZ = 2048;

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

layout(binding = DROPLETS_SSBO_BINDING) buffer droplet_data {
	droplet_data_t	d[];
} droplets;

layout(binding = VERTICES_SSBO_BINDING) buffer droplet_vtx {
	droplet_vtx_t	v[];
} vertices;

layout(binding = TAILS_SSBO_BINDING) buffer droplet_tails {
	droplet_tail_t	t[];
} tails;

void droplet_tail_construct(void);

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
bump_droplet(void)
{
	const float bump_sz_slow = 60.0;
	const float bump_sz_fast = 1.0;
	const float spd_fract = smoothstep(0.0, 0.2, DROPLET.spd);
	const float velocity_mul = 0.3;//, 0.8, spd_fract);
	const float bump_check_rate =
	    mix(0.2, 0.1, smoothstep(0.0, 0.2, DROPLET.spd));
	const float bump_prob_min = 0.25;
	const float bump_prob_max = 0.75;
	float rand_val, scale, bump_prob, spd_clamped;

	if (cur_t - DROPLET.bump_t < bump_check_rate)
		return;
	DROPLET.bump_t = cur_t;
	spd_clamped = clamp(DROPLET.spd, 0.0, 1.0);
	bump_prob = mix(bump_prob_min, bump_prob_max, spd_clamped);
	if (gold_noise(DROPLET.pos[0], rand_seed[0]) < (1.0 - bump_prob))
		return;
	rand_val = gold_noise(DROPLET.pos[0], rand_seed[1]);
	scale = mix(bump_sz_slow, bump_sz_fast, spd_clamped) / DEPTH_TEX_SZ;

	DROPLET.bump_sz = scale * (rand_val - 0.5);
	DROPLET.bump_rate = gold_noise(DROPLET.pos[0], rand_seed[1]);
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
	float spd = length(DROPLET.velocity);
	float rand_val, prob;

	coeff_vel = clamp(spd, 0.0, 0.01);

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
	bump_droplet();

	new_pos = DROPLET.pos[0] + DROPLET.velocity * d_t;

	if (DROPLET.bump_sz > 0.0) {
		vec2 velocity_norm = vec2_norm_right(DROPLET.velocity);
		float bump_sz = min(spd * d_t * DROPLET.bump_rate,
		    DROPLET.bump_sz);

		DROPLET.bump_sz -= bump_sz;
		new_pos += normalize(velocity_norm) * bump_sz;
	} else if (DROPLET.bump_sz < 0.0) {
		vec2 velocity_norm = vec2_norm_right(DROPLET.velocity);
		float bump_sz = max(-spd * d_t, DROPLET.bump_sz);

		DROPLET.bump_sz -= bump_sz;
		new_pos += normalize(velocity_norm) * bump_sz;
	}

	if (DROPLET.streamer) {
		if (cur_t - DROPLET.tail_upd_t > 0.1) {
			for (int i = NUM_DROPLET_HISTORY - 1; i > 0; i--)
				DROPLET.pos[i] = DROPLET.pos[i - 1];
			DROPLET.tail_upd_t = cur_t;
		}
/*
		float lag = 0.1 +
		    (gl_LocalInvocationIndex / (15.0 * DROPLET_WG_SIZE));
		for (int i = NUM_DROPLET_HISTORY - 1; i > 0; i--) {
			FILTER_IN(DROPLET.pos[i], DROPLET.pos[i - 1], d_t, lag);
		}
*/
		DROPLET.pos[0] = new_pos;
	} else {
		for (int i = NUM_DROPLET_HISTORY - 1; i > 0; i--)
			DROPLET.pos[i] = DROPLET.pos[0];
		DROPLET.pos[0] = new_pos;
	}

	if (length(DROPLET.pos[0] - DROPLET.pos[1]) > 0.0001) {
		vec2 tail_v = DROPLET.pos[0] - DROPLET.pos[1];
		FILTER_IN(DROPLET.tail_v, tail_v, d_t, 0.2);
	}
	FILTER_IN(DROPLET.spd, spd, d_t, 0.2);
}

void
droplet_init_velocity(vec2 droplet_pos)
{
	/* gravity, thrust and wind */
	vec2 F_a, F_g, F_t, F_w;


	F_g = normalize(droplet_pos - gravity_point) * gravity_force;
	F_t = normalize(droplet_pos - thrust_point) * thrust_force;
	F_w = normalize(droplet_pos - wind_point) * wind_force;
	F_a = (F_g + F_t + F_w);

	DROPLET.velocity = F_a * gold_noise(droplet_pos, rand_seed[3]);

	if (length(F_a) > 0.0)
		DROPLET.tail_angle = -dir2hdg(F_a);
}

void
droplet_regen(void)
{
	vec2 coord, droplet_pos;
	float droplet_sz_ratio;

	if (cur_t - DROPLET.regen_t < REGEN_DELAY)
		return;
	DROPLET.regen_t = cur_t;

	coord = vec2(gl_GlobalInvocationID);
	if (gold_noise(coord, rand_seed[4]) > precip_intens)
		return;

	droplet_pos = vec2(gold_noise(coord, rand_seed[5]),
	    gold_noise(coord, rand_seed[6]));
	for (int i = 0; i < NUM_DROPLET_HISTORY; i++)
		DROPLET.pos[i] = droplet_pos;
	droplet_sz_ratio = gold_noise(coord, rand_seed[7]);
	DROPLET.quant = mix(min_droplet_sz, MAX_DROPLET_SZ, droplet_sz_ratio);
	DROPLET.bump_sz = 0.0;

	droplet_init_velocity(droplet_pos);

	DROPLET.streamer = ((gl_GlobalInvocationID.x & 7) == 0);

	for (int i = 0; i < NUM_DROPLET_HISTORY; i++)
		TAIL(i).pos = vec2(0.0);
}

void
fast_line(vec2 p1, vec2 p2, float width, float depth1, float depth2)
{
	vec2 dp = p2 - p1;
	float dp_len = length(dp);
	vec2 dir = normalize(dp);
	vec2 right = vec2_norm_right(dir);

	for (float l = -width; l <= width; l += 1.0) {
		vec2 ps = p1 + right * l;
		float len;

		for (vec2 p = vec2(0.0); (len = length(p)) <= dp_len;
		    p += dir) {
			float depth = mix(depth1, depth2, len / dp_len);
			depth_store(ivec2(ps + p), depth);
		}
	}
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
	} else if (DROPLET.quant < 0.1 * MAX_DROPLET_SZ
	    /* || very_fast_droplet || DROPLET.streamer*/) {
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

	fast_line(vec2(DROPLET.pos[0] * DEPTH_TEX_SZ),
	    vec2(DROPLET.pos[1] * DEPTH_TEX_SZ), 2.0, 1.0, 0.8);
	fast_line(vec2(DROPLET.pos[1] * DEPTH_TEX_SZ),
	    vec2(DROPLET.pos[2] * DEPTH_TEX_SZ), 2.0, 0.8, 0.6);
	fast_line(vec2(DROPLET.pos[2] * DEPTH_TEX_SZ),
	    vec2(DROPLET.pos[3] * DEPTH_TEX_SZ), 2.0, 0.6, 0.4);
	fast_line(vec2(DROPLET.pos[3] * DEPTH_TEX_SZ),
	    vec2(DROPLET.pos[4] * DEPTH_TEX_SZ), 2.0, 0.4, 0.3);
	fast_line(vec2(DROPLET.pos[4] * DEPTH_TEX_SZ),
	    vec2(DROPLET.pos[5] * DEPTH_TEX_SZ), 2.0, 0.3, 0.25);
	fast_line(vec2(DROPLET.pos[5] * DEPTH_TEX_SZ),
	    vec2(DROPLET.pos[6] * DEPTH_TEX_SZ), 2.0, 0.25, 0.2);
	fast_line(vec2(DROPLET.pos[6] * DEPTH_TEX_SZ),
	    vec2(DROPLET.pos[7] * DEPTH_TEX_SZ), 0.0, 0.2, 0.0);

#if 0

	back_v = (DROPLET.pos[1] - DROPLET.pos[0]) * DEPTH_TEX_SZ;
	back_v_len = length(back_v);
	back_v_norm = normalize(back_v) * 0.5;
	right = vec2_norm_right(back_v_norm);
	left = -right;
	if (very_fast_droplet) {
		for (vec2 v = back_v_norm; length(v) < back_v_len;
		    v += back_v_norm) {
			depth_store(img_pos + ivec2(v), 1.0);
			depth_store(img_pos + ivec2(v + left), 0.8);
			depth_store(img_pos + ivec2(v + right), 0.8);
		}
	} else {
		for (vec2 v = back_v_norm; length(v) < back_v_len;
		    v += back_v_norm) {
			depth_store(img_pos + ivec2(v), 1.0);
			depth_store(img_pos + ivec2(v + left), 0.9);
			depth_store(img_pos + ivec2(v + 2.0 * left), 0.7);
			depth_store(img_pos + ivec2(v + 3.0 * left), 0.25);
			depth_store(img_pos + ivec2(v + right), 0.9);
			depth_store(img_pos + ivec2(v + 2.0 * right), 0.7);
			depth_store(img_pos + ivec2(v + 3.0 * right), 0.25);
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
			depth_store(img_pos + ivec2(v), 1.0);
			depth_store(img_pos + ivec2(v + left), 0.5);
			depth_store(img_pos + ivec2(v + 2.0 * left), 0.25);
			depth_store(img_pos + ivec2(v + right), 0.5);
			depth_store(img_pos + ivec2(v + 2.0 * right), 0.25);
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
#endif
}

#define	VTX_ANGLE(i)	(2.0 * M_PI * LINSTEP(0, FACES_PER_DROPLET, i))
const vec3 vtx_pos[VTX_PER_DROPLET - 1] = vec3[VTX_PER_DROPLET - 1](
    vec3(sin(VTX_ANGLE(0)), cos(VTX_ANGLE(0)) - 0.75, 1.0),
    vec3(sin(VTX_ANGLE(1)), cos(VTX_ANGLE(1)) - 0.75, 1.0),
    vec3(sin(VTX_ANGLE(2)), cos(VTX_ANGLE(2)) - 0.75, 1.0),
    vec3(sin(VTX_ANGLE(3)), cos(VTX_ANGLE(3)) - 0.75, 1.0),
    vec3(sin(VTX_ANGLE(4)), cos(VTX_ANGLE(4)) - 0.75, 1.0),
    vec3(sin(VTX_ANGLE(5)), cos(VTX_ANGLE(5)) - 0.75, 1.0),
    vec3(sin(VTX_ANGLE(6)), cos(VTX_ANGLE(6)) - 0.75, 1.0),
    vec3(sin(VTX_ANGLE(7)), cos(VTX_ANGLE(7)) - 0.75, 1.0),
    vec3(sin(VTX_ANGLE(8)), cos(VTX_ANGLE(8)) - 0.75, 1.0)
);

void
droplet_vtx_construct(void)
{
	float quant_fract = sqrt(DROPLET.quant / MAX_DROPLET_SZ);
	float radius = (10.0 * quant_fract) / DEPTH_TEX_SZ;
	float spd_fract = LINSTEP(0.0, 0.05, DROPLET.spd);
	vec2 scale_factor =
	    mix(vec2(1.0, 1.0), vec2(0.8, 2.0), spd_fract) * radius;
	float angle = 0.0;
	mat3 xform;

	if (DROPLET.tail_v != vec2(0.0))
		angle = -dir2hdg(DROPLET.tail_v);

	xform = mat3_rotate(mat3_scale(mat3(1.0), scale_factor), angle);

	for (int i = 0; i < VTX_PER_DROPLET - 1; i++) {
		VERTEX(i).pos = vec2((xform * vtx_pos[i]));
		VERTEX(i).ctr = DROPLET.pos[0];
		VERTEX(i).radius = radius;
		VERTEX(i).size = radius;
	}
	VERTEX(VTX_PER_DROPLET - 1).pos = vec2(0.0);
	VERTEX(VTX_PER_DROPLET - 1).ctr = DROPLET.pos[0];
	VERTEX(VTX_PER_DROPLET - 1).radius = 0.0;
	VERTEX(VTX_PER_DROPLET - 1).size = radius;
}

void
droplet_tail_construct(void)
{
	for (int i = 0; i < NUM_DROPLET_HISTORY; i++) {
		TAIL(i).pos = DROPLET.pos[i];
		TAIL(i).quant =
		    DROPLET.quant * (1.0 - (float(i) / NUM_DROPLET_HISTORY));
	}
}

bool
droplet_should_exist(void)
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
		droplet_vtx_construct();
		if (DROPLET.streamer)
			droplet_tail_construct();
	} else {
		/* Regen droplet at new location */
		droplet_regen();
	}
}
