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

layout(location = 0) uniform mat4	pvm;

layout(location = 0) in vec3		vtx_pos;
layout(location = 1) in vec3		vtx_norm;
layout(location = 2) in vec2		vtx_tex0;

layout(location = 0) out vec3		tex_norm;
layout(location = 1) out vec2		tex_coord;

void
main()
{
	tex_norm = vtx_norm;
	tex_coord = vtx_tex0;
	gl_Position = pvm * vec4(vtx_pos, 1.0);
}
