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

#define	C2KELVIN(c)	((c) + 273.15)
#define	POW2(x)		((x) * (x))

float
fx_lin(float x, float x1, float y1, float x2, float y2)
{
	return (((x - x1) / (x2 - x1)) * (y2 - y1) + y1);
}
