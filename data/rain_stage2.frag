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
uniform vec2		my_tex_sz;
uniform vec2		tp;
uniform float		thrust;

const float max_depth = 3;

float
read_depth(vec2 pos)
{
	vec4 val = texture2D(tex, pos / my_tex_sz);
	return (val.r + val.g + val.g);
}

void
main()
{
	float depth_left, depth_right, depth_up, depth_down;
	float d_lr, d_ud;
	vec2 thrust_v = (gl_FragCoord.xy - tp);

	thrust_v /= length(thrust_v);
	thrust_v *= 10 * thrust + 1;

	depth_left = read_depth(gl_FragCoord.xy + vec2(-1, 0) * thrust_v);
	depth_right = read_depth(gl_FragCoord.xy + vec2(1, 0) * thrust_v);
	depth_up = read_depth(gl_FragCoord.xy + vec2(0, 1) * thrust_v);
	depth_down = read_depth(gl_FragCoord.xy + vec2(0, -1) * thrust_v);

	d_lr = (atan(depth_left - depth_right) / (3.1415 / 2)) + 0.5;
	d_ud = (atan(depth_up - depth_down) / (3.1415 / 2)) + 0.5;

	gl_FragColor = vec4(d_lr, d_ud, 0.0, 1.0);
}
