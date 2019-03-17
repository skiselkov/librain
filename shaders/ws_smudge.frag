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

layout(location = 10) uniform sampler2D	depth_tex;
layout(location = 11) uniform sampler2D	screenshot_tex;
layout(location = 12) uniform vec2	screenshot_tex_sz;
layout(location = 13) uniform sampler2D	ws_tex;
layout(location = 14) uniform vec4	vp;

layout(location = 0) in vec3		tex_norm;
layout(location = 1) in vec2		tex_coord;

layout(location = 0) out vec4		color_out;

#define	KERNEL	float[25]( \
	0.01, 0.02, 0.04, 0.02, 0.01, \
	0.02, 0.04, 0.08, 0.04, 0.02, \
	0.04, 0.08, 0.16, 0.08, 0.04, \
	0.02, 0.04, 0.08, 0.04, 0.02, \
	0.01, 0.02, 0.04, 0.02, 0.01 \
)

vec4
get_pixel(vec2 pos)
{
	vec4 pixel;

	pos = pos / screenshot_tex_sz;
	pos = clamp(pos, vec2(0), vec2(vp.zw / screenshot_tex_sz) - 0.001);

	return (texture(ws_tex, pos));
}

void
main()
{
	vec4 depth_val = texture(depth_tex, tex_coord);
	float depth = depth_val.r;
	float depth_rat = depth / max_depth;

#define	BLUR_I(x, y, coeff_i) \
	(get_pixel(gl_FragCoord.xy - vp.xy + \
	    depth_rat * vec2(float(x), float(y))) * KERNEL[coeff_i])

	color_out =
	    /* row 0 */
	    BLUR_I(-2, -2, 0) +
	    BLUR_I(-1, -2, 1) +
	    BLUR_I(0, -2, 2) +
	    BLUR_I(1, -2, 3) +
	    BLUR_I(2, -2, 4) +
	    /* row 1 */
	    BLUR_I(-2, -1, 5) +
	    BLUR_I(-1, -1, 6) +
	    BLUR_I(0, -1, 7) +
	    BLUR_I(1, -1, 8) +
	    BLUR_I(2, -1, 9) +
	    /* row 2 */
	    BLUR_I(-2, 0, 10) +
	    BLUR_I(-1, 0, 11) +
	    BLUR_I(0, 0, 12) +
	    BLUR_I(1, 0, 13) +
	    BLUR_I(2, 0, 14) +
	    /* row 3 */
	    BLUR_I(-2, 1, 15) +
	    BLUR_I(-1, 1, 16) +
	    BLUR_I(0, 1, 17) +
	    BLUR_I(1, 1, 18) +
	    BLUR_I(2, 1, 19) +
	    /* row 3 */
	    BLUR_I(-2, 2, 20) +
	    BLUR_I(-1, 2, 21) +
	    BLUR_I(0, 2, 22) +
	    BLUR_I(1, 2, 23) +
	    BLUR_I(2, 2, 24);
}
