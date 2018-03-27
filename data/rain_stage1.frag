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

uniform	sampler2D	tex;
uniform float		rand_seed;
uniform float		precip_intens;
uniform float		thrust;
uniform float		d_t;
uniform vec2		tp;		/* thrust origin point */
uniform vec2		gp;		/* gravity origin point */
uniform float		gravity;

/*
 * Gold Noise ©2017-2018 dcerisano@standard3d.com 
 *  - based on the Golden Ratio, PI and Square Root of Two
 *  - fastest noise generator function
 *  - works with all chipsets (including low precision)
 */

precision lowp    float;

const float PHI = 1.61803398874989484820459 * 00000.1;	/* Golden Ratio */
const float PI  = 3.14159265358979323846264 * 00000.1;	/* PI */
const float SQ2 = 1.41421356237309504880169 * 10000.0;	/* Square Root of Two */
const float max_depth = 3.0;
const float precip_fact = 0.25;
const float gravity_factor = 0.05;

float
gold_noise(vec2 coordinate, float seed)
{
	return fract(sin(dot(coordinate * (seed + PHI), vec2(PHI, PI))) * SQ2);
}

bool
droplet_gen_check(vec2 pos)
{
	return (gold_noise(pos, rand_seed) > (1 - precip_fact *
	    precip_intens * max(pow(1 - thrust, 6), 0.25) * d_t));
}

float
read_depth(vec2 pos)
{
	vec4 val = texture2D(tex, pos / textureSize2D(tex, 0));
	return (val.r + val.b + val.g);
}

void
main()
{
	float old_depth, depth, prev_depth, new_depth;
	vec2 tex_sz = textureSize2D(tex, 0);
	vec2 prev_pos;
	vec2 tp_dir, gp_dir, rand_dir;
	vec4 old_val;
	float r = 0, g = 0, b = 0, a = 0;
	float blowaway_fact;

	if (droplet_gen_check(gl_FragCoord.xy)) {
		new_depth = max_depth;
	} else if (droplet_gen_check(gl_FragCoord.xy + vec2(1, 0)) ||
	    droplet_gen_check(gl_FragCoord.xy + vec2(-1, 0)) ||
	    droplet_gen_check(gl_FragCoord.xy + vec2(0, 1)) ||
	    droplet_gen_check(gl_FragCoord.xy + vec2(0, -1))) {
		new_depth = max_depth / 3;
	} else {
		new_depth = 0.0;
	}

	depth += new_depth;

	old_depth = read_depth(gl_FragCoord.xy);
	depth += old_depth * 0.4;

	gp_dir = (gl_FragCoord.xy - gp);
	gp_dir = gp_dir / length(gp_dir);

	tp_dir = (gl_FragCoord.xy - tp);
	tp_dir = tp_dir / length(tp_dir);

	rand_dir = vec2(sin(fract(gl_FragCoord.x / tex_sz.x + rand_seed)),
	    sin(fract(gl_FragCoord.y / tex_sz.y + rand_seed)));

	prev_pos = gl_FragCoord.xy -
	    ((gp_dir * (gravity_factor * (gravity + precip_intens)) +
	    tp_dir * thrust) * tex_sz * d_t);
	if (depth < 0.05)
		prev_pos += rand_dir;
	prev_depth = read_depth(prev_pos);

	blowaway_fact = 0.6 - thrust * 0.05 + (0.012 - precip_intens * 0.012);

	if (prev_depth > max_depth / 4)
		depth += prev_depth * (blowaway_fact + 0.5);
	else
		depth += prev_depth * (blowaway_fact - 0.01);

	depth = clamp(depth, 0.0, max_depth);
	if (depth <= 1.0)
		r = clamp(depth, 0.0, 1.0);
	else if (depth > 1.0 && depth <= 2.0)
		g = clamp(depth - 1.0, 0.0, 1.0);
	else
		b = clamp(depth - 2.0, 0.0, 1.0);

	gl_FragColor = vec4(r, g, b, 1.0);
}
