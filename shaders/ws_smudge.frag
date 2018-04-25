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
layout(location = 11) uniform sampler2D	screenshot_tex;
layout(location = 12) uniform sampler2D	ws_tex;

layout(location = 0) in vec3		tex_norm;
layout(location = 1) in vec2		tex_coord;

layout(location = 0) out vec4		color_out;

const float	max_depth = 3.0;
const float	kernel[25] = float[25](
	0.01, 0.02, 0.04, 0.02, 0.01,
	0.02, 0.04, 0.08, 0.04, 0.02,
	0.04, 0.08, 0.16, 0.08, 0.04,
	0.02, 0.04, 0.08, 0.04, 0.02,
	0.01, 0.02, 0.04, 0.02, 0.01
);

vec4
get_pixel(vec2 pos)
{
	vec4 pixel;

	pos = pos / textureSize(ws_tex, 0);
	pos = clamp(pos, 0.0, 0.99999);

	pixel = texture(ws_tex, pos);
	if (pixel.a == 1.0)
		return (pixel);
	else
		return (texture(screenshot_tex, pos));
}

void
main()
{
	vec4 depth_val = texture(depth_tex, tex_coord);
	float depth = depth_val.r;
	float depth_rat = depth / max_depth;
	float depth_rat_fact = 10 * pow(depth_rat, 1.2);
	vec4 out_pixel = vec4(0, 0, 0, 0);

	for (float x = 0; x < 5; x++) {
		for (float y = 0; y < 5; y++) {
			vec4 pixel = get_pixel(gl_FragCoord.xy +
			    depth_rat_fact * vec2(x - 2, y - 2));

			if (pixel.a != 0.0)
				out_pixel += kernel[int(y * 5 + x)] * pixel;
			else
				discard;
		}
	}

	color_out = vec4(out_pixel.rgb, 1);
}
