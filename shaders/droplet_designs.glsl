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
#define	ROW_OF_1(y)	PX(0, y, 1.0)
#define	ROW_OF_3(y)	PX(-1, y, 0.5), PX(0, y, 1.0), PX(1, y, 0.5)
#define	ROW_OF_5(y)	PX(-2, y, 0.33), PX(-1, y, 0.67), PX(0, y, 1.0), \
	PX(1, y, 0.67), PX(2, y, 0.33)
#define	ROW_OF_7(y)	PX(-3, y, 0.25), PX(-2, y, 0.5), PX(-1, y, 0.75), \
    PX(0, y, 1.0), PX(1, y, 0.75), PX(2, y, 0.5), PX(3, y, 0.25)

#define	ROW_OF_9(y)	PX(-4, y, 0.2), PX(-3, y, 0.6), PX(-2, y, 0.8), \
    PX(-1, y, 0.9), PX(0, y, 1.0), PX(1, y, 0.9), \
    PX(2, y, 0.8), PX(3, y, 0.6), PX(4, y, 0.2)

#define	ROW_OF_11(y) PX(-5, y, 0.16), PX(-4, y, 0.33), PX(-3, y, 0.5), \
    PX(-2, y, 0.6), PX(-1, y, 0.83), PX(0, y, 1.0), PX(1, y, 0.83), \
    PX(2, y, 0.66), PX(3, y, 0.5), PX(4, y, 0.33), PX(5, y, 0.16)

#define	ROW_OF_13(y)	\
    PX(-6, y, 0.1), PX(-5, y, 0.3), PX(-4, y, 0.5), \
    PX(-3, y, 0.75), PX(-2, y, 0.9), PX(-1, y, 0.95), \
    PX(0, y, 1.0), \
    PX(1, y, 0.95), PX(2, y, 0.9), PX(3, y, 0.75), PX(4, y, 0.5), \
    PX(5, y, 0.3), PX(6, y, 0.1)

struct droplet_pixel_t {
    vec2	pos;
    float	depth;
};

const droplet_pixel_t droplet_style1[] = droplet_pixel_t[](
    ROW_OF_1(1),
    ROW_OF_3(0),
    ROW_OF_1(-1)
);

const droplet_pixel_t droplet_style2[] = droplet_pixel_t[](
    ROW_OF_3(2),
    ROW_OF_5(1),
    ROW_OF_5(0),
    ROW_OF_5(-1),
    ROW_OF_3(-2)
);

const droplet_pixel_t droplet_style3[] = droplet_pixel_t[](
    ROW_OF_1(5),
    ROW_OF_1(4),
    ROW_OF_3(3),
    ROW_OF_5(2),
    ROW_OF_7(1),
    ROW_OF_7(0),
    ROW_OF_7(-1),
    ROW_OF_5(-2),
    ROW_OF_3(-3)
);

const droplet_pixel_t droplet_style3_tail[] = droplet_pixel_t[](
    ROW_OF_1(-7),
    ROW_OF_1(-6),
    ROW_OF_3(-5),
    ROW_OF_3(-4)
);

const droplet_pixel_t droplet_style4[] = droplet_pixel_t[](
    ROW_OF_5(4),
    ROW_OF_7(3),
    ROW_OF_9(2),
    ROW_OF_9(1),
    ROW_OF_9(0),
    ROW_OF_9(-1),
    ROW_OF_9(-2),
    ROW_OF_7(-3),
    ROW_OF_5(-4)
);

const droplet_pixel_t droplet_style4_tail[] = droplet_pixel_t[](
    ROW_OF_1(-9),
    ROW_OF_1(-8),
    ROW_OF_3(-7),
    ROW_OF_3(-6),
    ROW_OF_5(-5)
);

const droplet_pixel_t droplet_style5[] = droplet_pixel_t[](
    ROW_OF_5(5),
    ROW_OF_9(4),
    ROW_OF_9(3),
    ROW_OF_11(2),
    ROW_OF_11(1),
    ROW_OF_11(0),
    ROW_OF_11(-1),
    ROW_OF_11(-2),
    ROW_OF_9(-3),
    ROW_OF_9(-4),
    ROW_OF_5(-5)
);

const droplet_pixel_t droplet_style5_tail[] = droplet_pixel_t[](
    ROW_OF_1(-10),
    ROW_OF_3(-9),
    ROW_OF_3(-8),
    ROW_OF_5(-7),
    ROW_OF_5(-6),
    ROW_OF_5(-5),
    ROW_OF_5(-4)
);

const droplet_pixel_t droplet_style6[] = droplet_pixel_t[](
    ROW_OF_5(7),
    ROW_OF_9(6),
    ROW_OF_9(5),
    ROW_OF_11(4),
    ROW_OF_11(3),
    ROW_OF_13(2),
    ROW_OF_13(1),
    ROW_OF_13(0),
    ROW_OF_13(-1),
    ROW_OF_13(-2),
    ROW_OF_11(-3),
    ROW_OF_11(-4),
    ROW_OF_5(-5),
    ROW_OF_9(-6),
    ROW_OF_5(-7)
);

const droplet_pixel_t droplet_style6_tail[] = droplet_pixel_t[](
    ROW_OF_1(-13),
    ROW_OF_3(-12),
    ROW_OF_5(-11),
    ROW_OF_5(-10),
    ROW_OF_7(-9),
    ROW_OF_9(-8),
    ROW_OF_9(-7),
    ROW_OF_9(-6)
);

#endif	/* _DROPLET_DESIGNS_GLSL_ */
