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
 * Copyright 2020 Saso Kiselkov. All rights reserved.
 */

#ifndef	_GLPRIV_H_
#define	_GLPRIV_H_

#include <XPLMGraphics.h>

#include <acfutils/core.h>
#include <acfutils/glew.h>
#include <acfutils/glutils.h>
#include <acfutils/shader.h>

#ifdef	__cplusplus
extern "C" {
#endif

#define	DESTROY_OP(var, zero_val, destroy_op) \
	do { \
		if ((var) != (zero_val)) { \
			destroy_op; \
			(var) = (zero_val); \
		} \
	} while (0)

static void setup_texture_filter(GLuint tex, GLint miplevels, GLint int_fmt,
    GLsizei width, GLsizei height, GLenum format, GLenum type,
    const GLvoid *data, GLint mag_filter, GLint min_filter) UNUSED_ATTR;
static void setup_texture(GLuint tex, GLint int_fmt, GLsizei width,
    GLsizei height, GLenum format, GLenum type, const GLvoid *data) UNUSED_ATTR;
static void setup_color_fbo_for_tex(GLuint fbo, GLuint tex, GLuint depth,
    GLuint stencil, bool_t depth_stencil_combo) UNUSED_ATTR;
static bool_t reload_gl_prog(GLint *prog, const shader_prog_info_t *info)
    UNUSED_ATTR;
static char	*shaderpath	UNUSED_ATTR;

static char	*shaderpath = NULL;

static void
setup_texture_filter(GLuint tex, GLint miplevels, GLint int_fmt, GLsizei width,
    GLsizei height, GLenum format, GLenum type, const GLvoid *data,
    GLint mag_filter, GLint min_filter)
{
	ASSERT(tex != 0);
	ASSERT3S(miplevels, >, 0);

	glBindTexture(GL_TEXTURE_2D, tex);

	if (mag_filter != 0) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
		    mag_filter);
	}
	if (min_filter != 0) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
		    min_filter);
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	if (GLEW_ARB_texture_storage) {
		/*
		 * On AMD, we MUST specify the mipmapping level
		 * explicitly, otherwise glGenerateMipmaps can hang.
		 * WARNING: can't do both glTexStorage2D and glTexImage2D
		 * later to populate one mip level.
		 */
		if (data == NULL) {
			glTexStorage2D(GL_TEXTURE_2D, miplevels, int_fmt,
			    width, height);
			glGenerateMipmap(GL_TEXTURE_2D);
		} else {
			ASSERT3S(miplevels, ==, 1);
			glTexImage2D(GL_TEXTURE_2D, 0, int_fmt, width, height,
			    0, format, type, data);
		}
	} else {
		glTexImage2D(GL_TEXTURE_2D, 0, int_fmt, width, height, 0,
		    format, type, data);
		for (GLint i = 1; i <= miplevels; i++) {
			glTexImage2D(GL_TEXTURE_2D, i, int_fmt,
			    MAX(width >> i, 1), MAX(height >> i, 1),
			    0, format, type, NULL);
		}
		if (miplevels > 0 && data != NULL)
			glGenerateMipmap(GL_TEXTURE_2D);
	}
	GLUTILS_ASSERT_NO_ERROR();
}

static void
setup_texture(GLuint tex, GLint int_fmt, GLsizei width, GLsizei height,
    GLenum format, GLenum type, const GLvoid *data)
{
	setup_texture_filter(tex, 1, int_fmt, width, height, format,
	    type, data, GL_LINEAR, GL_LINEAR);
}

static void
setup_color_fbo_for_tex(GLuint fbo, GLuint tex, GLuint depth, GLuint stencil,
    bool_t depth_stencil_combo)
{
	glBindFramebufferEXT(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_TEXTURE_2D, tex, 0);
	if (depth_stencil_combo) {
		ASSERT(depth != 0);
		glFramebufferTexture2D(GL_FRAMEBUFFER,
		    GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
	} else {
		if (depth != 0) {
			glFramebufferTexture2D(GL_FRAMEBUFFER,
			    GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
		}
		if (stencil != 0) {
			glFramebufferTexture2D(GL_FRAMEBUFFER,
			    GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, stencil, 0);
		}
	}
	VERIFY3U(glCheckFramebufferStatus(GL_FRAMEBUFFER), ==,
	    GL_FRAMEBUFFER_COMPLETE);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
	    GL_STENCIL_BUFFER_BIT);
	glBindFramebufferEXT(GL_FRAMEBUFFER, 0);
}

static bool_t
reload_gl_prog(GLint *prog, const shader_prog_info_t *info)
{
	GLint new_prog;

	ASSERT(shaderpath != NULL);
	new_prog = shader_prog_from_info(shaderpath, info);
	if (new_prog == 0)
		return (B_FALSE);
	if (*prog != 0 && new_prog != *prog)
		glDeleteProgram(*prog);
	*prog = new_prog;

	return (B_TRUE);
}

static inline void
gl_state_cleanup(void)
{
	if (GLEW_VERSION_3_0)
		glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	GLUTILS_ASSERT_NO_ERROR();
}

#ifdef	__cplusplus
}
#endif

#endif	/* _GLPRIV_H_ */
