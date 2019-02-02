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

#ifndef	_DROPLETS_DATA_H_
#define	_DROPLETS_DATA_H_

#include "glsl_hdrs.h"

#define	DROPLET_WG_SIZE		1024
#define	NUM_DROPLET_HISTORY	8
#define	NUM_RANDOM_SEEDS	8

STRUCT(droplet_data_t, {
	/* 0-bit boundary */
	vec2	pos[NUM_DROPLET_HISTORY];
	/* 512-bit boundary */
	vec2	velocity;
	float	quant;
	float	regen_t;
	float	bump_t;
	float	F_d_s_len;
	float	bump_sz;
	float	tail_angle;	/* Kelvin */
	bool	streamer;
});

#endif	/* _DROPLETS_DATA_H_ */
