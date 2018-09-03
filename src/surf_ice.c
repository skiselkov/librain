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

#include <stdlib.h>
#include <string.h>

#include <GL/glew.h>
#include <cglm/cglm.h>

#include <acfutils/crc64.h>
#include <acfutils/dr.h>
#include <acfutils/glutils.h>
#include <acfutils/png.h>
#include <acfutils/safe_alloc.h>
#include <acfutils/time.h>

#include "glpriv.h"
#include "librain.h"
#include "surf_ice.h"

#define	ICE_RENDER_MAX_DELAY	10

struct surf_ice_impl_s {
	int		cur;
	double		prev_ice;
	GLuint		depth_tex[2];
	GLuint		depth_fbo[2];
	GLuint		norm_tex;
	GLuint		norm_fbo;
	glutils_quads_t	quads;
	obj8_t		*obj;
	char		*group_id;
	uint64_t	last_ice_t;
	double		prev_render_t;
	mat4		pvm;
};

static bool_t inited = B_FALSE;
static GLint point_depth_add_prog = 0;
static GLint point_depth_rem_prog = 0;
static GLint line_depth_add_prog = 0;
static GLint line_depth_rem_prog = 0;
static GLint norm_prog = 0;
static GLint render_norm_prog = 0;
static GLint render_blur_prog = 0;
static double cur_real_t = 0;
static double last_ice_real_t = 0;

static shader_info_t generic_vert_info = { .filename = "generic.vert.spv" };
static shader_info_t depth_vert_info = { .filename = "ice_depth.vert.spv" };
static shader_info_t point_depth_add_frag_info =
    { .filename = "ice_depth_add_point.frag.spv" };
static shader_info_t point_depth_rem_frag_info =
    { .filename = "ice_depth_remove_point.frag.spv" };
static shader_info_t line_depth_add_frag_info =
    { .filename = "ice_depth_add_line.frag.spv" };
static shader_info_t line_depth_rem_frag_info =
    { .filename = "ice_depth_remove_line.frag.spv" };
static shader_info_t norm_frag_info = { .filename = "ice_norm.frag.spv" };
static shader_info_t render_frag_info = { .filename = "ice_render.frag.spv" };
static shader_info_t render_blur_frag_info =
    { .filename = "ice_render_blur.frag.spv" };
static shader_prog_info_t point_depth_add_prog_info = {
    .progname = "ice_depth_add_point",
    .vert = &generic_vert_info,
    .frag = &point_depth_add_frag_info
};
static shader_prog_info_t point_depth_rem_prog_info = {
    .progname = "ice_depth_remove_point",
    .vert = &generic_vert_info,
    .frag = &point_depth_rem_frag_info
};
static shader_prog_info_t line_depth_add_prog_info = {
    .progname = "ice_depth_add_line",
    .vert = &generic_vert_info,
    .frag = &line_depth_add_frag_info
};
static shader_prog_info_t line_depth_rem_prog_info = {
    .progname = "ice_depth_remove_line",
    .vert = &generic_vert_info,
    .frag = &line_depth_rem_frag_info
};
static shader_prog_info_t norm_prog_info = {
    .progname = "ice_norm",
    .vert = &generic_vert_info,
    .frag = &norm_frag_info
};
static shader_prog_info_t render_norm_prog_info = {
    .progname = "ice_render_norm",
    .vert = &depth_vert_info,
    .frag = &render_frag_info
};
static shader_prog_info_t render_blur_prog_info = {
    .progname = "ice_render_blur",
    .vert = &depth_vert_info,
    .frag = &render_blur_frag_info
};

static struct {
	dr_t	viewport;
	dr_t	sim_time;
} drs;

bool_t
surf_ice_glob_init(const char *the_shaderpath)
{
	ASSERT(!inited);
	inited = B_TRUE;

	shaderpath = strdup(the_shaderpath);
	if (!surf_ice_reload_gl_progs())
		goto errout;

	fdr_find(&drs.viewport, "sim/graphics/view/viewport");
	fdr_find(&drs.sim_time, "sim/time/total_running_time_sec");

	return (B_TRUE);
errout:
	surf_ice_glob_fini();
	return (B_FALSE);
}

void
surf_ice_glob_fini(void)
{
	if (!inited)
		return;
	inited = B_FALSE;
	free(shaderpath);
	shaderpath = NULL;
	DESTROY_OP(point_depth_add_prog, 0,
	    glDeleteProgram(point_depth_add_prog));
	DESTROY_OP(point_depth_rem_prog, 0,
	    glDeleteProgram(point_depth_rem_prog));
	DESTROY_OP(line_depth_add_prog, 0,
	    glDeleteProgram(line_depth_add_prog));
	DESTROY_OP(line_depth_rem_prog, 0,
	    glDeleteProgram(line_depth_rem_prog));
	DESTROY_OP(norm_prog, 0, glDeleteProgram(norm_prog));
	DESTROY_OP(render_norm_prog, 0, glDeleteProgram(render_norm_prog));
	DESTROY_OP(render_blur_prog, 0, glDeleteProgram(render_blur_prog));
}

void
surf_ice_init(surf_ice_t *surf, obj8_t *obj, const char *group_id)
{
	surf_ice_impl_t *priv;
	vect2_t p[] = {
	    VECT2(0, 0),
	    VECT2(0, surf->h),
	    VECT2(surf->w, surf->h),
	    VECT2(surf->w, 0)
	};
	vect2_t t[] = { VECT2(0, 0), VECT2(0, 1), VECT2(1, 1), VECT2(1, 0) };
	GLfloat temp_tex[surf->w * surf->h];

	ASSERT(inited);
	ASSERT(obj != NULL);
	ASSERT3U(surf->w, >, 0);
	ASSERT3U(surf->h, >, 0);

	priv = safe_calloc(1, sizeof (*priv));
	surf->priv = priv;

	glGenTextures(2, priv->depth_tex);
	glGenFramebuffers(2, priv->depth_fbo);
	memset(temp_tex, 0, sizeof (temp_tex));
	for (int i = 0; i < 2; i++) {
		setup_texture(priv->depth_tex[i], GL_R16F, surf->w, surf->h,
		    GL_RED, GL_HALF_FLOAT, temp_tex);
		setup_color_fbo_for_tex(priv->depth_fbo[i], priv->depth_tex[i]);
	}
	glGenTextures(1, &priv->norm_tex);
	glGenFramebuffers(1, &priv->norm_fbo);
	setup_texture(priv->norm_tex, GL_RG, surf->w, surf->h,
	    GL_RG, GL_UNSIGNED_BYTE, NULL);
	setup_color_fbo_for_tex(priv->norm_fbo, priv->norm_tex);

	glutils_init_2D_quads(&priv->quads, p, t, 4);

	priv->obj = obj;
	if (group_id != NULL)
		priv->group_id = strdup(group_id);
	glm_ortho(0, surf->w, 0, surf->h, 0, 1, priv->pvm);
}

void
surf_ice_fini(surf_ice_t *surf)
{
	surf_ice_impl_t *priv = surf->priv;

	if (priv == NULL)
		return;
	DESTROY_OP(priv->depth_fbo[0], 0,
	    glDeleteFramebuffers(2, priv->depth_fbo));
	DESTROY_OP(priv->depth_tex[0], 0, glDeleteTextures(2, priv->depth_tex));
	DESTROY_OP(priv->norm_fbo, 0, glDeleteFramebuffers(1, &priv->norm_fbo));
	DESTROY_OP(priv->norm_tex, 0, glDeleteTextures(1, &priv->norm_tex));
	glutils_destroy_quads(&priv->quads);
	free(priv->group_id);

	free(priv);
}

bool_t
surf_ice_reload_gl_progs(void)
{
	return (reload_gl_prog(&norm_prog, &norm_prog_info) &&
	    reload_gl_prog(&render_norm_prog, &render_norm_prog_info) &&
	    reload_gl_prog(&render_blur_prog, &render_blur_prog_info) &&
	    reload_gl_prog(&point_depth_add_prog, &point_depth_add_prog_info) &&
	    reload_gl_prog(&point_depth_rem_prog, &point_depth_rem_prog_info) &&
	    reload_gl_prog(&line_depth_add_prog, &line_depth_add_prog_info) &&
	    reload_gl_prog(&line_depth_rem_prog, &line_depth_rem_prog_info));
}

static void
update_depth(surf_ice_t *surf, double d_t, double ice, bool_t deice_on)
{
	GLint old_fbo;
	int vp[4];
	GLint depth_prog;
	double d_ice;
	surf_ice_impl_t *priv = surf->priv;

	UNUSED(deice_on);

	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_fbo);
	VERIFY3S(dr_getvi(&drs.viewport, vp, 0, 4), ==, 4);
	glViewport(0, 0, surf->w, surf->h);

	glBindFramebuffer(GL_FRAMEBUFFER, priv->depth_fbo[priv->cur]);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);

	d_ice = ice - priv->prev_ice;
	if (d_ice > 0) {
		if (surf->src == SURF_ICE_SRC_POINT)
			depth_prog = point_depth_add_prog;
		else
			depth_prog = line_depth_add_prog;
	} else {
		if (surf->src == SURF_ICE_SRC_POINT)
			depth_prog = point_depth_rem_prog;
		else
			depth_prog = line_depth_rem_prog;
	}
	ASSERT(depth_prog != 0);

	glUseProgram(depth_prog);
	glActiveTexture(GL_TEXTURE0);
	XPLMBindTexture2d(priv->depth_tex[!priv->cur], GL_TEXTURE_2D);
	glUniformMatrix4fv(glGetUniformLocation(depth_prog, "pvm"), 1,
	    GL_FALSE, (void *)priv->pvm);
	glUniform1i(glGetUniformLocation(depth_prog, "prev"), 0);
	glUniform1f(glGetUniformLocation(depth_prog, "ice"), ice);
	glUniform1f(glGetUniformLocation(depth_prog, "d_ice"), d_ice);
	glUniform1f(glGetUniformLocation(depth_prog, "d_t"), d_t);
	glUniform1f(glGetUniformLocation(depth_prog, "seed"),
	    crc64_rand_fract());
	glutils_draw_quads(&priv->quads, depth_prog);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, priv->norm_fbo);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glUseProgram(norm_prog);
	XPLMBindTexture2d(priv->depth_tex[priv->cur], GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(norm_prog, "depth"), 0);
	glUniformMatrix4fv(glGetUniformLocation(norm_prog, "pvm"), 1,
	    GL_FALSE, (void *)priv->pvm);
	glutils_draw_quads(&priv->quads, norm_prog);

	glViewport(vp[0], vp[1], vp[2], vp[3]);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_fbo);

	priv->prev_ice = ice;
}

void
surf_ice_render(surf_ice_t *surf, double ice, bool_t deice_on,
    double blur_radius)
{
	surf_ice_impl_t *priv = surf->priv;
	double d_t;
	double cur_sim_t = dr_getf(&drs.sim_time);
	mat4 pvm;
	GLint render_prog;

	UNUSED(deice_on);
	ASSERT(priv != NULL);

	if (priv->prev_render_t == 0)
		priv->prev_render_t = cur_sim_t;
	d_t = cur_sim_t - priv->prev_render_t;

	if (ice > 0) {
		priv->last_ice_t = cur_sim_t;
		last_ice_real_t = cur_real_t;
	} else if (ice == 0 &&
	    cur_sim_t - priv->last_ice_t > ICE_RENDER_MAX_DELAY) {
		return;
	}

	if (d_t > 0.1 && ABS(ice - priv->prev_ice) > 0.0001) {
		update_depth(surf, d_t, ice, deice_on);
		priv->cur = !priv->cur;
		priv->prev_render_t = cur_sim_t;
	}

	if (blur_radius == 0)
		render_prog = render_norm_prog;
	else
		render_prog = render_blur_prog;
	glUseProgram(render_prog);

	glUniformMatrix4fv(glGetUniformLocation(render_prog, "pvm"), 1,
	    GL_FALSE, (void *)priv->pvm);
	glActiveTexture(GL_TEXTURE0);
	XPLMBindTexture2d(priv->depth_tex[priv->cur], GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(render_prog, "depth"), 0);

	if (blur_radius != 0) {
		glUniform1f(glGetUniformLocation(render_prog, "rand_seed"),
		    crc64_rand_fract());
		glUniform1f(glGetUniformLocation(render_prog, "blur_radius"),
		    blur_radius);
	}

	glActiveTexture(GL_TEXTURE1);
	XPLMBindTexture2d(priv->norm_tex, GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(render_prog, "norm"), 1);

	glActiveTexture(GL_TEXTURE2);
	XPLMBindTexture2d(librain_get_screenshot_tex(), GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(render_prog, "bg"), 2);

	librain_get_pvm(pvm);
	glDepthMask(GL_FALSE);
	obj8_draw_group(priv->obj, priv->group_id, render_prog, pvm);
	glDepthMask(GL_TRUE);

	glUseProgram(0);
	glActiveTexture(GL_TEXTURE0);
}

void
surf_ice_clear(surf_ice_t *surf)
{
	surf_ice_impl_t *priv = surf->priv;
	GLfloat depth_buf[surf->w * surf->h];
	uint16_t norm_buf[surf->w * surf->h];

	ASSERT(priv != NULL);

	memset(depth_buf, 0, sizeof (depth_buf));
	memset(norm_buf, 0, sizeof (norm_buf));

	for (int i = 0; i < 2; i++) {
		XPLMBindTexture2d(priv->depth_tex[i], GL_TEXTURE_2D);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, surf->w, surf->h, 0,
		    GL_RED, GL_FLOAT, depth_buf);
	}
	XPLMBindTexture2d(priv->norm_tex, GL_TEXTURE_2D);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RG, surf->w, surf->h, 0,
	    GL_RG, GL_UNSIGNED_BYTE, norm_buf);
}

bool_t
surf_ice_render_pass_needed(void)
{
	return (cur_real_t - last_ice_real_t < ICE_RENDER_MAX_DELAY);
}

void
surf_ice_render_pass_begin(void)
{
	cur_real_t = USEC2SEC(microclock());
}

void
surf_ice_render_pass_done(void)
{
	/*
	 * Since the rendered ice display can be slightly behind what the
	 * application notified us, we keep rendering our
	 */
	if (surf_ice_render_pass_needed())
		librain_refresh_screenshot();
}
