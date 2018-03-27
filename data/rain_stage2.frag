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

	depth_left = read_depth(vec2(gl_FragCoord.x - 1, gl_FragCoord.y));
	depth_right = read_depth(vec2(gl_FragCoord.x + 1, gl_FragCoord.y));
	depth_up = read_depth(vec2(gl_FragCoord.x, gl_FragCoord.y + 1));
	depth_down = read_depth(vec2(gl_FragCoord.x, gl_FragCoord.y - 1));

	d_lr = (atan(depth_left - depth_right) / (3.1415 / 2)) + 0.5;
	d_ud = (atan(depth_up - depth_down) / (3.1415 / 2)) + 0.5;

	gl_FragColor = vec4(d_lr, d_ud, 0.0, 1.0);
}
