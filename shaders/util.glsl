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

#ifndef	_UTIL_GLSL_
#define	_UTIL_GLSL_

#define	C2KELVIN(c)	((c) + 273.15)
#define	POW2(x)		((x) * (x))
#define	POW3(x)		((x) * (x) * (x))
#define	POW4(x)		(POW2(x) * POW2(x))

#define	FILTER_IN(old_val, new_val, d_t, lag) \
	old_val = mix((old_val), (new_val), (d_t) / (lag))
#define	LINSTEP(edge0, edge1, x) \
	clamp((float(x) - float(edge0)) / (float(edge1) - float(edge0)), \
	    0.0, 1.0)

float
fx_lin(float x, float x1, float y1, float x2, float y2)
{
	return (((x - x1) / (x2 - x1)) * (y2 - y1) + y1);
}

float
dir2hdg(vec2 v)
{
	if (v.y >= 0.0)
		return (asin(v.x / length(v)));
	else if (v.x >= 0.0)
		return (radians(180) - asin(v.x / length(v)));
	else
		return (radians(-180) - asin(v.x / length(v)));
}

float
rel_hdg(float hdg1, float hdg2)
{
	if (hdg1 > hdg2) {
		if (hdg1 > hdg2 + 180)
			return (radians(360) - hdg1 + hdg2);
		else
			return (-(hdg1 - hdg2));
	} else {
		if (hdg2 > hdg1 + radians(180))
			return (-(radians(360) - hdg2 + hdg1));
		else
			return (hdg2 - hdg1);
	}
}

float
normalize_rot(float angle)
{
	return (mod(angle, radians(180)));
}

float
iter_fract(float x, float start, float end)
{
	return ((x - start) / (end - start));
}

vec2
vec2_norm_right(vec2 v)
{
	return (vec2(v.y, -v.x));
}

#endif	/* _UTIL_GLSL_ */
