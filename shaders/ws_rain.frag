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

#version 460

layout(location = 10) uniform sampler2D	depth_tex;
layout(location = 11) uniform sampler2D	norm_tex;
layout(location = 12) uniform sampler2D	screenshot_tex;

layout(location = 0) in vec3		tex_norm;
layout(location = 1) in vec2		tex_coord;

layout(location = 0) out vec4	color_out;

const float	displace_lim = 200.0;
const float	darkening_fact = 450.0;
const float	max_depth = 3.0;

vec4
get_pixel(vec2 pos)
{
	pos = pos / textureSize(screenshot_tex, 0);
	pos = clamp(pos, 0.0, 0.99999);
	return (texture(screenshot_tex, pos));
}

void
main()
{
	float depth = texture(depth_tex, tex_coord).r;
	vec2 displace = (texture(norm_tex, tex_coord).xy - 0.5) *
	    displace_lim * (max_depth - depth);
	vec4 bg_pixel = get_pixel(gl_FragCoord.xy + displace);

	bg_pixel *= (1 - pow(length(displace) / darkening_fact, 1.8));

	color_out = vec4(bg_pixel.rgb, 1.0);
}
