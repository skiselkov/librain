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
	(tails.t[(DROPLET_ID >> STREAMER_SHIFT) * NUM_DROPLET_HISTORY + (_idx)])

#define	FAST_SPD_LIM		0.1	/* tex/sec */
#define	VERY_FAST_SPD_LIM	0.2	/* tex/sec */

layout(local_size_x = DROPLET_WG_SIZE) in;

layout(constant_id = DEPTH_TEX_SZ_CONSTANT_ID) const int DEPTH_TEX_SZ = 2048;

const float tail_update_rate = 0.05;
const vec2 droplet_deform_scale = vec2(0.8, 2.0);

#define	VTX_ANGLE(i)	(2.0 * M_PI * LINSTEP(0, FACES_PER_DROPLET, i))
#define	DROPLET_Y_OFF	0.6
const vec3 vtx_pos[VTX_PER_DROPLET - 1] = vec3[VTX_PER_DROPLET - 1](
    vec3(sin(VTX_ANGLE(0)), cos(VTX_ANGLE(0)) - DROPLET_Y_OFF, 1.0),
    vec3(sin(VTX_ANGLE(1)) * 1.25, cos(VTX_ANGLE(1)) - DROPLET_Y_OFF, 1.0),
    vec3(sin(VTX_ANGLE(2)), cos(VTX_ANGLE(2)) - DROPLET_Y_OFF, 1.0),
    vec3(sin(VTX_ANGLE(3)) * 0.75, cos(VTX_ANGLE(3)) - DROPLET_Y_OFF, 1.0),
    vec3(sin(VTX_ANGLE(4)) * 0.75, cos(VTX_ANGLE(4)) - DROPLET_Y_OFF * 1.2,
    1.0),
    vec3(sin(VTX_ANGLE(5)) * 0.75, cos(VTX_ANGLE(5)) - DROPLET_Y_OFF * 1.2,
    1.0),
    vec3(sin(VTX_ANGLE(6)) * 0.75, cos(VTX_ANGLE(6)) - DROPLET_Y_OFF, 1.0),
    vec3(sin(VTX_ANGLE(7)), cos(VTX_ANGLE(7)) - DROPLET_Y_OFF, 1.0),
    vec3(sin(VTX_ANGLE(8)) * 1.25, cos(VTX_ANGLE(8)) - DROPLET_Y_OFF, 1.0)
);

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

vec2
droplet_forces_integrate(vec2 old_pos)
{
	vec2 droplet_pos = old_pos;
	/* gravity, thrust and wind */
	vec2 F_a, F_g, F_t, F_w;

	F_g = normalize(droplet_pos - gravity_point) * gravity_force;
	F_t = normalize(droplet_pos - thrust_point) * thrust_force;
	F_w = normalize(droplet_pos - wind_point) * wind_force;

	F_a = (F_g + F_t + F_w);

	return (F_a);
}

float
bump_rate_upd(vec2 pos)
{
	const float bump_rate_check_rate = 0.05;
	float new_rate;

	if (cur_t - DROPLET.bump_rate_upd_t < bump_rate_check_rate)
		return (DROPLET.bump_rate);
	new_rate = gold_noise(pos, rand_seed[1]);
	DROPLET.bump_rate_upd_t = cur_t;
	DROPLET.bump_rate = new_rate;

	return (new_rate);
}

vec2
bump_droplet(vec2 pos, float spd, vec2 velocity, out float bump_sz_out)
{
	float bump_sz;
	const float bump_sz_slow = 20.0;
	const float bump_sz_fast = 1.0;
	const float spd_fract = smoothstep(0.0, 0.2, spd);
	const float velocity_mul = 0.7;
	const float bump_check_rate = 0.05;
	const float bump_prob_min = 0.25;
	const float bump_prob_max = 0.75;
	float rand_val, scale, bump_prob, spd_clamped;

	if (cur_t - DROPLET.bump_t < bump_check_rate) {
		bump_sz_out = DROPLET.bump_sz;
		return (velocity);
	}
	DROPLET.bump_t = cur_t;
	spd_clamped = clamp(spd, 0.0, 1.0);
	bump_prob = mix(bump_prob_min, bump_prob_max, spd_clamped);
	if (gold_noise(pos, rand_seed[0]) < (1.0 - bump_prob)) {
		bump_sz_out = DROPLET.bump_sz;
		return (velocity);
	}
	rand_val = gold_noise(pos, rand_seed[1]);
	scale = mix(bump_sz_slow, bump_sz_fast, spd_clamped) / DEPTH_TEX_SZ;

	bump_sz = scale * (rand_val - 0.5);
	DROPLET.bump_sz = bump_sz;
	bump_sz_out = bump_sz;

	return (velocity * velocity_mul);
}

void
streamer_upd_tail(void)
{
	if (cur_t - DROPLET.tail_upd_t > tail_update_rate) {
		for (int i = NUM_DROPLET_HISTORY - 1; i > 0; i--)
			DROPLET.pos[i] = DROPLET.pos[i - 1];
		DROPLET.tail_upd_t = cur_t;
	}
}

void
update_droplet_tail_v(float spd, vec2 old_pos, vec2 new_pos, out vec2 new_tail)
{
	vec2 tail_v, tail_v_tgt;
	vec2 d_pos = new_pos - old_pos;
	float rate = 1.0 - clamp(smoothstep(0.0, 0.02, spd), 0.0, 0.9);

	d_pos = new_pos - old_pos;
	if (d_pos != vec2(0.0))
		tail_v_tgt = normalize(d_pos);
	else
		tail_v_tgt = vec2(0.0, -1.0);
	tail_v = mix(DROPLET.tail_v, tail_v_tgt, d_t / rate);

	DROPLET.tail_v = tail_v;
	new_tail = tail_v;
}

float
update_static_drag_len(vec2 pos, float quant, float spd)
{
	float F_d_s_len = DROPLET.F_d_s_len;
	float F_d_s_len_tgt, rand_val, prob;
	float coeff_vel = clamp(spd, 0.0, 0.01);

	F_d_s_len_tgt = mix(MAX_DRAG_STATIC, MIN_DRAG_STATIC, coeff_vel) /
	    max(quant, 1.0);
	FILTER_IN(F_d_s_len, F_d_s_len_tgt, d_t, 0.5);
	rand_val = gold_noise(pos, rand_seed[2]);
	prob = mix(MIN_RANDOM_DROP_RATE, MAX_RANDOM_DROP_RATE, precip_intens);
	if (rand_val > prob)
		F_d_s_len = 0.0;

	return (F_d_s_len);
}

void
droplet_move(float quant, bool streamer, out vec2 new_pos_out,
    out float spd_out, out vec2 new_tail)
{
	/* Copy in local variables to avoid global memory access */
	float rel_quant = quant / MAX_DROPLET_SZ;
	vec2 old_pos = DROPLET.pos[0];
	vec2 velocity = DROPLET.velocity;
	float spd = DROPLET.spd;
	float spd_tgt = length(velocity);
	/* acceleration force */
	vec2 F_a = droplet_forces_integrate(old_pos);
	/* drag force - dynamic */
	vec2 F_d_d = -velocity * COEFF_DRAG_DYN;
	/* drag force - static */
	vec2 F_d_s;
	/* final force */
	vec2 F;
	vec2 new_pos, tail_v;
	float F_d_s_len, rand_val, prob, bump_sz;

	FILTER_IN(spd, spd_tgt, d_t, 0.2);
	F_d_s_len = update_static_drag_len(old_pos, quant, spd);

	if (F_d_s_len < length(F_a))
		F_d_s = normalize(F_a) * (-F_d_s_len);
	else
		F_d_s = -F_a;

	F = F_a + F_d_d + F_d_s;
	velocity += F * d_t;
	velocity = bump_droplet(old_pos, spd, velocity, bump_sz);
	new_pos = velocity * d_t + old_pos;

	if (bump_sz > 0.0) {
		vec2 velocity_norm;
		float displace, bump_rate;

		bump_rate = bump_rate_upd(old_pos);
		velocity_norm = vec2_norm_right(velocity);
		displace = min(spd * d_t * bump_rate, bump_sz);
		new_pos += normalize(velocity_norm) * displace;
		bump_sz -= displace;
	} else if (bump_sz < 0.0) {
		vec2 velocity_norm;
		float displace, bump_rate;

		bump_rate = bump_rate_upd(old_pos);
		velocity_norm = vec2_norm_right(velocity);
		displace = max(-spd * d_t * bump_rate, bump_sz);
		new_pos += normalize(velocity_norm) * displace;
		bump_sz -= displace;
	}
	DROPLET.bump_sz = bump_sz;

	/*
	 * Output update phase.
	 */
	if (streamer) {
		streamer_upd_tail();
		DROPLET.pos[0] = new_pos;
	} else {
		DROPLET.pos[1] = old_pos;
		DROPLET.pos[0] = new_pos;
	}
	new_pos_out = new_pos;
	spd_out = spd;

	DROPLET.velocity = velocity;
	DROPLET.F_d_s_len = F_d_s_len;

	update_droplet_tail_v(spd, old_pos, new_pos, tail_v);
	new_tail = tail_v;
	DROPLET.spd = spd;
	spd_out = spd;
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
	if (gold_noise(coord, rand_seed[4]) >= precip_intens)
		return;

	droplet_pos = vec2(gold_noise(coord, rand_seed[5]),
	    gold_noise(coord, rand_seed[6]));
	for (int i = 0; i < NUM_DROPLET_HISTORY; i++)
		DROPLET.pos[i] = droplet_pos;
	droplet_sz_ratio = gold_noise(coord, rand_seed[7]);
	DROPLET.quant = mix(min_droplet_sz, MAX_DROPLET_SZ, droplet_sz_ratio);
	DROPLET.bump_sz = 0.0;

	droplet_init_velocity(droplet_pos);

	DROPLET.streamer =
	    ((gl_GlobalInvocationID.x & ((1 << STREAMER_SHIFT) - 1)) == 0);

	if (DROPLET.streamer) {
		/*
		 * ONLY streamer droplets have tails. This avoids the
		 * costly vertex shader pass of giving tons of tail line
		 * segments to non-streamer droplets.
		 */
		for (int i = 0; i < NUM_DROPLET_HISTORY; i++)
			TAIL(i).pos = vec2(0.0);
	}
}

void
droplet_visual_construct(vec2 pos, float spd, float quant, bool streamer,
    vec2 tail_v)
{
	float spd_fract = LINSTEP(0.0, 0.1, spd);
	float quant_vis = mix(quant, quant / 2.0, spd_fract);
	float quant_fract = quant_vis / MAX_DROPLET_SZ;
	float radius = (10.0 * quant_fract) / DEPTH_TEX_SZ;
	vec2 scale_factor = mix(vec2(1.0), droplet_deform_scale, spd_fract) *
	    radius;
	const mat3 xform = mat3_rotate_dir(mat3_scale(mat3(1.0), scale_factor),
	    DROPLET.tail_v);

	for (int i = 0; i < VTX_PER_DROPLET - 1; i++) {
		VERTEX(i).pos = vec2((xform * vtx_pos[i]));
		VERTEX(i).ctr = pos;
		VERTEX(i).radius = radius;
		VERTEX(i).size = radius;
	}
	VERTEX(VTX_PER_DROPLET - 1).pos = vec2(0.0);
	VERTEX(VTX_PER_DROPLET - 1).ctr = pos;
	VERTEX(VTX_PER_DROPLET - 1).radius = 0.0;
	VERTEX(VTX_PER_DROPLET - 1).size = radius;

	if (streamer) {
		for (int i = 0; i < NUM_DROPLET_HISTORY; i++) {
			TAIL(i).pos = DROPLET.pos[i];
			TAIL(i).quant = quant_vis *
			    (1.0 - (float(i) / NUM_DROPLET_HISTORY));
		}
	}
}

bool
droplet_should_exist(float quant, bool streamer)
{
	vec2 pos;

	if (quant <= 0.0)
		return (false);
	if (streamer)
		pos = DROPLET.pos[NUM_DROPLET_HISTORY - 1];
	else
		pos = DROPLET.pos[0];

	return (pos.x > 0.0 && pos.x < 1.0 && pos.y > 0.0 && pos.y < 1.0);
}

void
main(void)
{
	float new_quant = DROPLET.quant - d_t;
	bool streamer = DROPLET.streamer;

	if (droplet_should_exist(new_quant, streamer)) {
		vec2 pos, tail_v;
		float spd;

		droplet_move(new_quant, streamer, pos, spd, tail_v);
		droplet_visual_construct(pos, spd, new_quant, streamer, tail_v);
		DROPLET.quant = new_quant;
	} else {
		/* Try to regen droplet at new location */
		droplet_regen();
	}
}
