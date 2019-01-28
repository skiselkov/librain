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

#ifndef	_GLSL_HDRS_H_
#define	_GLSL_HDRS_H_

#ifdef	__STDC_VERSION__

#define	STRUCT(__name, __def)	typedef struct __def __name

#else	/* !__STDC_VERSION__ */

#define	STRUCT(__name, __def)	struct __name __def

#endif	/* !__STDC_VERSION__ */

#endif	/* _GLSL_HDRS_H_ */
