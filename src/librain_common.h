/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license in the file COPYING
 * or http://www.opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file COPYING.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2020 Saso Kiselkov. All rights reserved.
 */

#ifndef	_LIBRAIN_COMMON_H_
#define	_LIBRAIN_COMMON_H_

#include "glpriv.h"

#ifdef	__cplusplus
extern "C" {
#endif

#if	defined(_WIN32) || defined(__CYGWIN__)
  #ifdef	DLLMODE
    #ifdef	__GNUC__
      #define	LIBRAIN_EXPORT	__attribute__((dllexport))
    #else
      #define	LIBRAIN_EXPORT	__declspec(dllexport)
    #endif
  #else		/* !DLLMODE */
    #ifdef	__GNUC__
      #define	LIBRAIN_EXPORT
    #else	/* !__GNUC__ */
      #define	LIBRAIN_EXPORT
    #endif	/* !__GNUC__ */
  #endif	/* !DLLMODE */
#else	/* !defined(_WIN32) && !defined(__CYGWIN__) */
  #ifdef	DLLMODE
    #define	LIBRAIN_EXPORT	__attribute__((visibility("default")))
  #else	/* !DLLMODE */
    #define	LIBRAIN_EXPORT
  #endif	/* !DLLMODE */
#endif

/*
 * librain internal
 */
GLint librain_get_current_fbo(void);
void librain_get_current_vp(GLint vp[4]);
void librain_reset_clip_control(void);

#ifdef	__cplusplus
}
#endif

#endif	/* _LIBRAIN_COMMON_H_ */
