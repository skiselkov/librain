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
uniform	sampler2D	screenshot_tex;
uniform	sampler2D	ws_tex;

const float		max_depth = 3.0;
const float		kernel[25] = float[25](
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

	pos = pos / textureSize2D(ws_tex, 0);
	pos = clamp(pos, 0.0, 0.99999);

	pixel = texture2D(ws_tex, pos);
	if (pixel.a == 1.0)
		return (pixel);
	else
		texture2D(screenshot_tex, pos);
}

void
main()
{
	vec4 depth_val = texture2D(depth_tex, gl_TexCoord[0].st);
	float depth = depth_val.r + depth_val.g + depth_val.b;
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

	gl_FragColor = vec4(out_pixel.rgb, 1);
}
