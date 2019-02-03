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

/* render pass inputs */
layout(location = 10) uniform sampler2D	depth;

/* from vertex shader */
layout(location = 0) in vec3		tex_norm;
layout(location = 1) in vec2		tex_coord;

/* frag shader output */
layout(location = 0) out vec4		color_out;

void
main()
{
	vec2 sz = textureSize(depth, 0);
	float x1 = texture(depth, vec2(max((gl_FragCoord.x - 1) / sz.x, 0.0),
	    gl_FragCoord.y / sz.y)).r;
	float x2 = texture(depth, vec2(min((gl_FragCoord.x + 1) / sz.x, 1.0),
	    gl_FragCoord.y / sz.y)).r;
	float y1 = texture(depth, vec2(gl_FragCoord.x / sz.x,
	    max((gl_FragCoord.y - 1) / sz.y, 0.0))).r;
	float y2 = texture(depth, vec2(gl_FragCoord.x / sz.x,
	    min((gl_FragCoord.y + 1) / sz.y, 1.0))).r;

	color_out = vec4((x2 - x1) / 2 + 0.5, (y2 - y1) / 2 + 0.5, 0, 1);
}
