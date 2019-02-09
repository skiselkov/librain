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

#include "consts.glsl"
#include "droplets_data.h"
#include "util.glsl"

#define	MAX_WIPERS	2
#define	RAIN_DEBUG	0

layout(location = 10) uniform sampler2D	depth_tex;
layout(location = 11) uniform sampler2D	norm_tex;
layout(location = 12) uniform sampler2D	screenshot_tex;
layout(location = 13) uniform vec4	vp;

layout(location = 14) uniform int	num_wipers;
layout(location = 15) uniform vec2	wiper_pivot[MAX_WIPERS];
layout(location = 17) uniform float	wiper_radius_outer[MAX_WIPERS];
layout(location = 19) uniform float	wiper_radius_inner[MAX_WIPERS];
layout(location = 21) uniform float	wiper_pos[MAX_WIPERS];

layout(location = 0) in vec3		tex_norm;
layout(location = 1) in vec2		tex_coord;

layout(location = 0) out vec4		color_out;

#if	!COMPUTE_VARIANT
const vec2	displace_lim = vec2(200.0);
const float	darkening_fact = 800.0;
#endif	/* !COMPUTE_VARIANT */

vec4
get_pixel(vec2 pos)
{
	vec2 sz = textureSize(screenshot_tex, 0);
	pos = pos / sz;
	pos = clamp(pos, vec2(0), vec2(vp.zw / sz) - 0.001);
	return (texture(screenshot_tex, pos));
}

bool
check_wiper(int i)
{
	vec2 wiper2pos = tex_coord - wiper_pivot[i];
	float dist = length(wiper2pos);
	float hdg;

	if (abs(dist - wiper_radius_outer[i]) < 0.002 ||
	    abs(dist - wiper_radius_inner[i]) < 0.002)
		return (true);

	hdg = dir2hdg(wiper2pos);
	if (dist <= wiper_radius_outer[i] &&
	    dist >= wiper_radius_inner[i] &&
	    abs(hdg - wiper_pos[i]) <= radians(1))
		return (true);

	return (false);
}

void
main()
{
#if	COMPUTE_VARIANT
	vec2 screenshot_sz = textureSize(screenshot_tex, 0);
	const vec2 displace_lim = screenshot_sz.yx / 10.0;
	float darkening_fact = max(displace_lim.x, displace_lim.y);
#endif
	float depth = texture(depth_tex, tex_coord).r;
	vec2 norm_v = (2.0 * texture(norm_tex, tex_coord).xy) - 1.0;
	vec2 displace = norm_v * displace_lim;
	vec4 bg_pixel = get_pixel(gl_FragCoord.xy + displace);
	float displace_fract = length(displace) / darkening_fact;

#if	RAIN_DEBUG
	color_out = vec4(depth, 0.0, 0.0, 1.0);
	return;
#endif

	bg_pixel *= (1.0 - POW2(displace_fract / 2.0));

	color_out = vec4(bg_pixel.rgb, 1.0);

	if (num_wipers > 0) {
		for (int i = 0; i < num_wipers; i++) {
			if (check_wiper(i)) {
				color_out += vec4(0.6, 0.0, 0.0, 0.0);
				break;
			}
		}
	}
}
