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

#ifndef	_GLPRIV_H_
#define	_GLPRIV_H_

#include <GL/glew.h>

#include <XPLMGraphics.h>

#include <acfutils/core.h>
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

static void setup_texture_filter(GLuint tex, GLint int_fmt, GLsizei width,
    GLsizei height, GLenum format, GLenum type, const GLvoid *data,
    GLint mag_filter, GLint min_filter) UNUSED_ATTR;
static void setup_texture(GLuint tex, GLint int_fmt, GLsizei width,
    GLsizei height, GLenum format, GLenum type, const GLvoid *data) UNUSED_ATTR;
static void setup_color_fbo_for_tex(GLuint fbo, GLuint tex) UNUSED_ATTR;
static bool_t reload_gl_prog(GLint *prog, const shader_prog_info_t *info,
    const shader_spec_const_t *sc_vert, const shader_spec_const_t *sc_frag,
    const shader_spec_const_t *sc_comp) UNUSED_ATTR;
static char	*shaderpath	UNUSED_ATTR;

static char	*shaderpath = NULL;

static void
setup_texture_filter(GLuint tex, GLint int_fmt, GLsizei width,
    GLsizei height, GLenum format, GLenum type, const GLvoid *data,
    GLint mag_filter, GLint min_filter)
{
	XPLMBindTexture2d(tex, GL_TEXTURE_2D);

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
	glTexImage2D(GL_TEXTURE_2D, 0, int_fmt, width, height, 0, format,
	    type, data);
}

static void
setup_texture(GLuint tex, GLint int_fmt, GLsizei width,
    GLsizei height, GLenum format, GLenum type, const GLvoid *data)
{
	setup_texture_filter(tex, int_fmt, width,
	    height, format, type, data, GL_LINEAR, GL_LINEAR);
}

static void
setup_color_fbo_for_tex(GLuint fbo, GLuint tex)
{
	glBindFramebufferEXT(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_TEXTURE_2D, tex, 0);
	VERIFY3U(glCheckFramebufferStatus(GL_FRAMEBUFFER), ==,
	    GL_FRAMEBUFFER_COMPLETE);
}

static bool_t
reload_gl_prog(GLint *prog, const shader_prog_info_t *in_info,
    const shader_spec_const_t *sc_vert, const shader_spec_const_t *sc_frag,
    const shader_spec_const_t *sc_comp)
{
	GLuint new_prog;
	shader_info_t vert, frag, comp;
	shader_prog_info_t info;

	memcpy(&info, in_info, sizeof (info));
	if (info.vert != NULL) {
		memcpy(&vert, info.vert, sizeof (vert));
		vert.spec_const = sc_vert;
		info.vert = &vert;
	}
	if (info.frag != NULL) {
		memcpy(&frag, info.frag, sizeof (frag));
		frag.spec_const = sc_frag;
		info.frag = &frag;
	}
	if (info.comp != NULL) {
		memcpy(&comp, info.comp, sizeof (comp));
		comp.spec_const = sc_comp;
		info.comp = &comp;
	}

	ASSERT(shaderpath != NULL);
	new_prog = shader_prog_from_info(shaderpath, &info);
	if (new_prog == 0)
		return (B_FALSE);
	if (*prog != 0)
		glDeleteProgram(*prog);
	*prog = new_prog;

	return (B_TRUE);
}

#ifdef	__cplusplus
}
#endif

#endif	/* _GLPRIV_H_ */
