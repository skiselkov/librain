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

uniform	sampler2D	depth_tex;
uniform	sampler2D	norm_tex;
uniform	sampler2D	screenshot_tex;

const float		displace_lim = 200.0;
const float		darkening_fact = 300.0;
const float		max_depth = 3.0;

vec4
get_pixel(vec2 pos)
{
	pos = pos / textureSize2D(screenshot_tex, 0);
	pos = clamp(pos, 0.0, 0.99999);
	return (texture2D(screenshot_tex, pos));
}

void
main()
{
	vec4 depth_val = texture2D(depth_tex, gl_TexCoord[0].st);
	float depth = depth_val.r + depth_val.g + depth_val.b;
	vec2 displace = (texture2D(norm_tex,
	    gl_TexCoord[0].st).xy - 0.5) * displace_lim * (max_depth - depth);
	vec4 bg_pixel = get_pixel(gl_FragCoord.xy + displace);

	bg_pixel *= (1 - pow(length(displace) / darkening_fact, 1.8));

	gl_FragColor = vec4(bg_pixel.rgb, 1.0);
}
