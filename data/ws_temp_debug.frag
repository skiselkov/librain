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

uniform	sampler2D	src;

void
main()
{
	float glass_temp = texture2D(src, gl_TexCoord[0].st).r * 400.0;

	gl_FragColor = vec4(
	    clamp((glass_temp - 300) / 50.0, 0, 1),
	    clamp((glass_temp - 250) / 50.0, 0, 1),
	    clamp((glass_temp - 200) / 50.0, 0, 1),
	    1.0);
}
