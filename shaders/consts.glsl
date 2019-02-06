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
 * Copyright 2019 Saso Kiselkov. All rights reserved.
 */

#ifndef	_CONSTS_GLSL_
#define	_CONSTS_GLSL_

#define	M_PI	radians(180)

#if	COMPUTE_VARIANT
const float max_depth = 1.0;
#else
const float max_depth = 3.0;
#endif

const float min_depth = 0.001;

#endif	/* _CONSTS_GLSL_ */
