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
#define	NUM_DROPLET_HISTORY	48
#define	NUM_DROPLET_TAIL_SEGS	(NUM_DROPLET_HISTORY - 1)
#define	NUM_RANDOM_SEEDS	8
#define	MAX_DROPLET_SZ		120
#define	MIN_DROPLET_SZ_MIN	20.0
#define	MIN_DROPLET_SZ_MAX	80.0

#define	VTX_PER_DROPLET		10
#define	FACES_PER_DROPLET	(VTX_PER_DROPLET - 1)
#define	STREAMER_SHIFT		4	/* for 15 short droplets, 1 streamer */
#define	NUM_STREAMERS(n_d)	((n_d) >> STREAMER_SHIFT)

#define	DEPTH_TEX_SZ_CONSTANT_ID	0
#define	NORM_TEX_SZ_CONSTANT_ID		1

#define	DROPLETS_SSBO_BINDING		0
#define	VERTICES_SSBO_BINDING		1
#define	TAILS_SSBO_BINDING		2

#define	STREAMER_WIDTH			20

STRUCT(droplet_data_t, {
	vec2	pos[NUM_DROPLET_HISTORY];	/* 32 x 64 bits */
	/* 128-bit boundary */
	vec2	velocity;	/* 64-bit */
	float	quant;		/* 32-bit */
	float	regen_t;	/* 32-bit */
	/* 128-bit boundary */
	vec2	tail_v;		/* 64-bit */
	float	bump_t;		/* 32-bit */
	float	F_d_s_len;	/* 32-bit */
	/* 128-bit boundary */
	float	bump_sz;	/* 32-bit */
	float	bump_rate;	/* 32-bit */
	float	bump_rate_upd_t;/* 32-bit */
	float	tail_angle;	/* 32-bit, radians */
	/* 128-bit boundary */
	float	spd;		/* 32-bit */
	bool	streamer;	/* 32-bit */
	float	tail_upd_t;	/* 32-bit */
	float	pad;		/* 32-bit */
	/* 128-bit boundary */
});

STRUCT(droplet_vtx_t, {
	vec2	pos;	/* 64-bit */
	vec2	ctr;	/* 64-bit */
	/* 128-bit boundary */
	float	radius;	/* 32-bit */
	float	size;	/* 32-bit */
	float	pad[2];	/* 96-bit */
	/* 128-bit boundary */
});

STRUCT(droplet_tail_t, {
	vec2	pos;	/* 64-bit */
	float	quant;	/* 32-bit */
	float	pad;	/* 32-bit */
	/* 128-bit boundary */
});

#endif	/* _DROPLETS_DATA_H_ */
