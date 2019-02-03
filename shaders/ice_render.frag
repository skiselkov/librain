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

/* render pass inputs */
layout(location = 10) uniform sampler2D	depth;
layout(location = 11) uniform sampler2D	norm;
layout(location = 12) uniform sampler2D	bg;
layout(location = 13) uniform vec3	sun_dir;
layout(location = 14) uniform float	sun_pitch;
layout(location = 15) uniform mat4	acf_orient;

/* from vertex shader */
layout(location = 0) in vec3		tex_norm;
layout(location = 1) in vec2		tex_coord;

/* frag shader output */
layout(location = 0) out vec4		color_out;

#define	POW4(x)	((x) * (x) * (x) * (x))

void
main()
{
	vec2 bg_sz = textureSize(bg, 0);
	vec4 bg_pixel = texture(bg, gl_FragCoord.xy / bg_sz);
	float white = bg_pixel.r + bg_pixel.g + bg_pixel.b;
	vec2 norm_pixel = texture(norm, tex_coord).rg - vec2(0.5);
	float depth_val = clamp(texture(depth, tex_coord).r, 0, 1.5);
	vec3 norm_dir = (acf_orient * vec4(tex_norm, 1)).xyz;
	float sun_angle = 1 - clamp(dot(norm_dir, sun_dir), 0, 1);
	float sun_darkening = sin(radians(clamp(sun_angle, 0, 90)));

	/*
	 * Because we don't know pixel lighting conditions, we need to work
	 * by boosting the brightness of the background pixel.
	 */

	color_out = vec4(1, 1, 1, (depth_val - length(norm_pixel) / 2 -
	    sun_angle / 2 - sun_darkening / 2) * sqrt(white));
}
