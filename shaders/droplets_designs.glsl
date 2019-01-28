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

#ifndef	_DROPLET_DESIGNS_GLSL_
#define	_DROPLET_DESIGNS_GLSL_

#define	PX(_x, _y, _d)	droplet_pixel_t(vec2(_x, _y), _d)

#define	ROW_OF_1(y, m)	PX(0, y, 1.0 * m)

#define	ROW_OF_3(y, m)	PX(-1, y, 0.5 * m), PX(0, y, 1.0 * m), \
	PX(1, y, 0.5 * m)

#define	ROW_OF_5(y, m)	PX(-2, y, 0.2 * m), PX(-1, y, 0.65 * m), \
	PX(0, y, 1.0 * m), PX(1, y, 0.65 * m), PX(2, y, 0.2 * m)

#define	ROW_OF_7(y, m)	PX(-3, y, 0.2 * m), PX(-2, y, 0.45 * m), \
	PX(-1, y, 0.8 * m), PX(0, y, 1.0 * m), PX(1, y, 0.8 * m), \
	PX(2, y, 0.45 * m), PX(3, y, 0.2 * m)

#define	ROW_OF_9(y, m)	PX(-4, y, 0.15 * m), PX(-3, y, 0.4 * m), \
	PX(-2, y, 0.7 * m), PX(-1, y, 0.9 * m), PX(0, y, 1.0 * m), \
	PX(1, y, 0.9 * m), PX(2, y, 0.7 * m), PX(3, y, 0.4 * m), \
	PX(4, y, 0.15 * m)

#define	ROW_OF_11(y, m)	PX(-5, y, 0.15 * m), PX(-4, y, 0.4 * m), \
	PX(-3, y, 0.6 * m), PX(-2, y, 0.8 * m), PX(-1, y, 0.95 * m), \
	PX(0, y, 1.0 * m), PX(1, y, 0.95 * m), PX(2, y, 0.8 * m), \
	PX(3, y, 0.6 * m), PX(4, y, 0.4 * m), PX(5, y, 0.15 * m)

#define	ROW_OF_13(y, m)	\
	PX(-6, y, 0.1 * m), PX(-5, y, 0.35 * m), PX(-4, y, 0.55 * m), \
	PX(-3, y, 0.75 * m), PX(-2, y, 0.9 * m), PX(-1, y, 0.95 * m), \
	PX(0, y, 1.0 * m), \
	PX(1, y, 0.95 * m), PX(2, y, 0.9 * m), PX(3, y, 0.75 * m), \
	PX(4, y, 0.55 * m), PX(5, y, 0.35 * m), PX(6, y, 0.1 * m)

struct droplet_pixel_t {
    vec2	pos;
    float	depth;
};

const droplet_pixel_t droplet_style1[] = droplet_pixel_t[](
    ROW_OF_1(1, 0.5),
    ROW_OF_3(0, 1.0),
    ROW_OF_1(-1, 0.5)
);

const droplet_pixel_t droplet_style2[] = droplet_pixel_t[](
    ROW_OF_3(2,		0.25),
    ROW_OF_5(1,		0.5),
    ROW_OF_5(0,		1.0),
    ROW_OF_5(-1,	0.5),
    ROW_OF_3(-2,	0.25)
);

const droplet_pixel_t droplet_style3[] = droplet_pixel_t[](
    ROW_OF_3(3,		0.45),
    ROW_OF_5(2,		0.75),
    ROW_OF_7(1,		0.9),
    ROW_OF_7(0,		1.0),
    ROW_OF_7(-1,	0.9),
    ROW_OF_5(-2,	0.75),
    ROW_OF_3(-3,	0.45)
);

const droplet_pixel_t droplet_style3_tail[] = droplet_pixel_t[](
    ROW_OF_1(-7,	0.1),
    ROW_OF_1(-6,	0.25),
    ROW_OF_3(-5,	0.35),
    ROW_OF_3(-4,	0.45)
);

const droplet_pixel_t droplet_style4[] = droplet_pixel_t[](
    ROW_OF_5(4,		0.3),
    ROW_OF_7(3,		0.65),
    ROW_OF_9(2,		0.8),
    ROW_OF_9(1,		0.9),
    ROW_OF_9(0,		1.0),
    ROW_OF_9(-1,	0.9),
    ROW_OF_9(-2,	0.8),
    ROW_OF_7(-3,	0.65),
    ROW_OF_5(-4,	0.3)
);

const droplet_pixel_t droplet_style4_tail[] = droplet_pixel_t[](
    ROW_OF_1(-9,	0.1),
    ROW_OF_1(-8,	0.1),
    ROW_OF_3(-7,	0.2),
    ROW_OF_3(-6,	0.2),
    ROW_OF_5(-5,	0.3)
);

const droplet_pixel_t droplet_style5[] = droplet_pixel_t[](
    ROW_OF_5(5,		0.2),
    ROW_OF_9(4,		0.4),
    ROW_OF_9(3,		0.65),
    ROW_OF_11(2,	0.8),
    ROW_OF_11(1,	0.9),
    ROW_OF_11(0,	1.0),
    ROW_OF_11(-1,	0.9),
    ROW_OF_11(-2,	0.8),
    ROW_OF_9(-3,	0.65),
    ROW_OF_9(-4,	0.4),
    ROW_OF_5(-5,	0.2)
);

const droplet_pixel_t droplet_style5_tail[] = droplet_pixel_t[](
    ROW_OF_1(-10,	0.1),
    ROW_OF_3(-9,	0.1),
    ROW_OF_3(-8,	0.2),
    ROW_OF_5(-7,	0.2),
    ROW_OF_5(-6,	0.3),
    ROW_OF_5(-5,	0.3),
    ROW_OF_5(-4,	0.3)
);

const droplet_pixel_t droplet_style6[] = droplet_pixel_t[](
    ROW_OF_5(7,		0.3),
    ROW_OF_9(6,		0.4),
    ROW_OF_9(5,		0.5),
    ROW_OF_11(4,	0.6),
    ROW_OF_11(3,	0.7),
    ROW_OF_13(2,	0.8),
    ROW_OF_13(1,	0.9),
    ROW_OF_13(0,	1.0),
    ROW_OF_13(-1,	0.9),
    ROW_OF_13(-2,	0.8),
    ROW_OF_11(-3,	0.7),
    ROW_OF_11(-4,	0.6),
    ROW_OF_9(-5,	0.5),
    ROW_OF_9(-6,	0.4),
    ROW_OF_5(-7,	0.3)
);

const droplet_pixel_t droplet_style6_tail[] = droplet_pixel_t[](
    ROW_OF_1(-13,	0.1),
    ROW_OF_3(-12,	0.1),
    ROW_OF_5(-11,	0.2),
    ROW_OF_5(-10,	0.3),
    ROW_OF_7(-9,	0.4),
    ROW_OF_9(-8,	0.4),
    ROW_OF_9(-7,	0.4),
    ROW_OF_9(-6,	0.3)
);

#endif	/* _DROPLET_DESIGNS_GLSL_ */
