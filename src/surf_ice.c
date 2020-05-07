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

#include <stdlib.h>
#include <string.h>

#include <acfutils/crc64.h>
#include <acfutils/dr.h>
#include <acfutils/glew.h>
#include <acfutils/glutils.h>
#include <acfutils/png.h>
#include <acfutils/safe_alloc.h>
#include <acfutils/time.h>

#include <cglm/cglm.h>

#include "glpriv.h"
#include "librain.h"
#include "surf_ice.h"

#define	ICE_RENDER_MAX_DELAY	60
#define	ICE_RESYNC_INTVAL	0.5

TEXSZ_MK_TOKEN(librain_ice_depth_tex);
TEXSZ_MK_TOKEN(librain_ice_norm_tex);
TEXSZ_MK_TOKEN(librain_ice_blur_tex);
TEXSZ_MK_TOKEN(librain_ice_packbuf);

struct surf_ice_impl_s {
	int		cur;
	double		prev_ice;
	double		act_ice;
	GLuint		depth_tex[2];
	GLuint		depth_fbo[2];
	GLuint		norm_tex;
	GLuint		norm_fbo;
	/*
	 * The blur textures are organized as follows:
	 * [0] contains the blur of the depth texture
	 * [1] contains the blur of the normal texture
	 */
	GLuint		blur_tex[2];
	GLuint		blur_fbo[2];
	GLuint		packbuf;
	GLsync		packbuf_sync;
	double		last_resync_t;
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
static GLint point_depth_deice_prog = 0;
static GLint line_depth_add_prog = 0;
static GLint line_depth_rem_prog = 0;
static GLint line_depth_deice_prog = 0;
static GLint norm_prog = 0;
static GLint render_prog = 0;
static GLint blur_prog = 0;
static double cur_real_t = 0;
static double last_ice_real_t = 0;
static mat4 acf_orient;
static vec3 sun_dir;

static shader_info_t generic_vert_info = { .filename = "generic.vert.spv" };
static shader_info_t depth_vert_info = { .filename = "ice_depth.vert.spv" };
static shader_info_t point_depth_add_frag_info =
    { .filename = "ice_depth_add_point.frag.spv" };
static shader_info_t point_depth_rem_frag_info =
    { .filename = "ice_depth_remove_point.frag.spv" };
static shader_info_t point_depth_deice_frag_info =
    { .filename = "ice_depth_deice_point.frag.spv" };
static shader_info_t line_depth_add_frag_info =
    { .filename = "ice_depth_add_line.frag.spv" };
static shader_info_t line_depth_rem_frag_info =
    { .filename = "ice_depth_remove_line.frag.spv" };
static shader_info_t line_depth_deice_frag_info =
    { .filename = "ice_depth_deice_line.frag.spv" };
static shader_info_t norm_frag_info = { .filename = "ice_norm.frag.spv" };
static shader_info_t render_frag_info = { .filename = "ice_render.frag.spv" };
static shader_info_t blur_frag_info =
    { .filename = "ice_blur.frag.spv" };
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
static shader_prog_info_t point_depth_deice_prog_info = {
    .progname = "ice_depth_deice_point",
    .vert = &generic_vert_info,
    .frag = &point_depth_deice_frag_info
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
static shader_prog_info_t line_depth_deice_prog_info = {
    .progname = "ice_depth_deice_line",
    .vert = &generic_vert_info,
    .frag = &line_depth_deice_frag_info
};
static shader_prog_info_t norm_prog_info = {
    .progname = "ice_norm",
    .vert = &generic_vert_info,
    .frag = &norm_frag_info
};
static shader_prog_info_t render_prog_info = {
    .progname = "ice_render",
    .vert = &depth_vert_info,
    .frag = &render_frag_info
};
static shader_prog_info_t blur_prog_info = {
    .progname = "ice_blur",
    .vert = &generic_vert_info,
    .frag = &blur_frag_info
};

static struct {
	dr_t	sim_time;
	dr_t	sun_pitch;
	dr_t	sun_hdg;
	dr_t	hdg;
	dr_t	pitch;
	dr_t	roll;
} drs;

bool_t
surf_ice_glob_init(const char *the_shaderpath)
{
	ASSERT(!inited);
	inited = B_TRUE;

	shaderpath = strdup(the_shaderpath);
	if (!surf_ice_reload_gl_progs())
		goto errout;

	fdr_find(&drs.sim_time, "sim/time/total_running_time_sec");
	fdr_find(&drs.sun_pitch, "sim/graphics/scenery/sun_pitch_degrees");
	fdr_find(&drs.sun_hdg, "sim/graphics/scenery/sun_heading_degrees");
	fdr_find(&drs.hdg, "sim/flightmodel/position/psi");
	fdr_find(&drs.pitch, "sim/flightmodel/position/theta");
	fdr_find(&drs.roll, "sim/flightmodel/position/phi");

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
	DESTROY_OP(point_depth_deice_prog, 0,
	    glDeleteProgram(point_depth_deice_prog));
	DESTROY_OP(line_depth_add_prog, 0,
	    glDeleteProgram(line_depth_add_prog));
	DESTROY_OP(line_depth_rem_prog, 0,
	    glDeleteProgram(line_depth_rem_prog));
	DESTROY_OP(line_depth_deice_prog, 0,
	    glDeleteProgram(line_depth_deice_prog));
	DESTROY_OP(norm_prog, 0, glDeleteProgram(norm_prog));
	DESTROY_OP(render_prog, 0, glDeleteProgram(render_prog));
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
	GLfloat *temp_tex;

	ASSERT(inited);
	ASSERT(obj != NULL);
	ASSERT3U(surf->w, >, 0);
	ASSERT3U(surf->h, >, 0);

	temp_tex = safe_calloc(sizeof (*temp_tex), surf->w * surf->h);

	priv = safe_calloc(1, sizeof (*priv));
	surf->priv = priv;

	glGenTextures(2, priv->depth_tex);
	glGenFramebuffers(2, priv->depth_fbo);
	for (int i = 0; i < 2; i++) {
		setup_texture(priv->depth_tex[i], GL_R32F, surf->w, surf->h,
		    GL_RED, GL_FLOAT, temp_tex);
		setup_color_fbo_for_tex(priv->depth_fbo[i], priv->depth_tex[i],
		    0, 0, B_FALSE);
		IF_TEXSZ(TEXSZ_ALLOC_INSTANCE(librain_ice_depth_tex, surf,
		    NULL, 0, GL_RED, GL_FLOAT, surf->w, surf->h));
	}
	glGenTextures(1, &priv->norm_tex);
	glGenFramebuffers(1, &priv->norm_fbo);
	setup_texture(priv->norm_tex, GL_RG8, surf->w, surf->h,
	    GL_RG, GL_UNSIGNED_BYTE, NULL);
	setup_color_fbo_for_tex(priv->norm_fbo, priv->norm_tex, 0, 0, B_FALSE);
	IF_TEXSZ(TEXSZ_ALLOC_INSTANCE(librain_ice_norm_tex, surf,
	    NULL, 0, GL_RG, GL_UNSIGNED_BYTE, surf->w, surf->h));

	glutils_init_2D_quads(&priv->quads, p, t, 4);

	priv->obj = obj;
	if (group_id != NULL)
		priv->group_id = strdup(group_id);
	glm_ortho(0, surf->w, 0, surf->h, 0, 1, priv->pvm);

	free(temp_tex);
}

void
surf_ice_fini(surf_ice_t *surf)
{
	surf_ice_impl_t *priv = surf->priv;

	if (priv == NULL)
		return;
	DESTROY_OP(priv->depth_fbo[0], 0,
	    glDeleteFramebuffers(2, priv->depth_fbo));
	if (priv->depth_tex[0] != 0) {
		for (int i = 0; i < 2; i++) {
			IF_TEXSZ(TEXSZ_FREE_INSTANCE(librain_ice_depth_tex,
			    surf, GL_RED, GL_FLOAT, surf->w, surf->h));
		}
	}
	if (priv->norm_tex != 0) {
		IF_TEXSZ(TEXSZ_FREE_INSTANCE(librain_ice_norm_tex, surf,
		    GL_RG, GL_UNSIGNED_BYTE, surf->w, surf->h));
	}
	if (priv->blur_tex[0] != 0) {
		for (int i = 0; i < 2; i++) {
			IF_TEXSZ(TEXSZ_FREE_INSTANCE(librain_ice_blur_tex,
			    surf, GL_RG, GL_UNSIGNED_BYTE, surf->w, surf->h));
		}
	}

	DESTROY_OP(priv->depth_tex[0], 0, glDeleteTextures(2, priv->depth_tex));
	DESTROY_OP(priv->norm_fbo, 0, glDeleteFramebuffers(1, &priv->norm_fbo));
	DESTROY_OP(priv->norm_tex, 0, glDeleteTextures(1, &priv->norm_tex));
	DESTROY_OP(priv->blur_fbo[0], 0,
	    glDeleteFramebuffers(2, priv->blur_fbo));
	DESTROY_OP(priv->blur_tex[0], 0, glDeleteTextures(2, priv->blur_tex));
	if (priv->packbuf != 0) {
		IF_TEXSZ(TEXSZ_FREE_BYTES_INSTANCE(librain_ice_packbuf,
		    surf, surf->w * surf->h * sizeof (GLfloat)));
	}
	DESTROY_OP(priv->packbuf, 0, glDeleteBuffers(1, &priv->packbuf));
	glutils_destroy_quads(&priv->quads);
	free(priv->group_id);

	free(priv);
}

bool_t
surf_ice_reload_gl_progs(void)
{
	return (reload_gl_prog(&norm_prog, &norm_prog_info) &&
	    reload_gl_prog(&render_prog, &render_prog_info) &&
	    reload_gl_prog(&blur_prog, &blur_prog_info) &&
	    reload_gl_prog(&point_depth_add_prog, &point_depth_add_prog_info) &&
	    reload_gl_prog(&point_depth_rem_prog, &point_depth_rem_prog_info) &&
	    reload_gl_prog(&point_depth_deice_prog,
	    &point_depth_deice_prog_info) &&
	    reload_gl_prog(&line_depth_add_prog, &line_depth_add_prog_info) &&
	    reload_gl_prog(&line_depth_rem_prog, &line_depth_rem_prog_info) &&
	    reload_gl_prog(&line_depth_deice_prog,
	    &line_depth_deice_prog_info));
}

static void
update_depth(surf_ice_t *surf, double cur_sim_t, double d_t, double ice,
    bool_t deice_on)
{
	GLint old_fbo;
	int vp[4];
	GLint depth_prog;
	double d_ice;
	surf_ice_impl_t *priv = surf->priv;

	if (ice == 0.0 && priv->prev_ice == 0.0 && !deice_on) {
		/* Final ice clearing phase, just keep removing ice */
		d_ice = -0.01;
	} else {
		d_ice = ice - priv->prev_ice;
	}
	/* If we're removing ice too quickly, wait a little */
	if (d_ice < 0.0001 && d_ice > -0.001)
		return;

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	/*
	 * For some reason our ortho protection breaks in reverser-Z,
	 * so fuck it and reset the clip control. We'll restore it later.
	 */
	glClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);

	old_fbo = librain_get_current_fbo();
	librain_get_current_vp(vp);
	glViewport(0, 0, surf->w, surf->h);

	glBindFramebufferEXT(GL_FRAMEBUFFER, priv->depth_fbo[priv->cur]);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);

	if (d_ice > 0) {
		if (surf->src == SURF_ICE_SRC_POINT)
			depth_prog = point_depth_add_prog;
		else
			depth_prog = line_depth_add_prog;
	} else {
		if (surf->src == SURF_ICE_SRC_POINT) {
			if (deice_on)
				depth_prog = point_depth_deice_prog;
			else
				depth_prog = point_depth_rem_prog;
		} else {
			if (deice_on)
				depth_prog = line_depth_deice_prog;
			else
				depth_prog = line_depth_rem_prog;
		}
	}
	ASSERT(depth_prog != 0);

	glUseProgram(depth_prog);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, priv->depth_tex[!priv->cur]);
	glUniformMatrix4fv(glGetUniformLocation(depth_prog, "pvm"), 1,
	    GL_FALSE, (void *)priv->pvm);
	glUniform1i(glGetUniformLocation(depth_prog, "prev"), 0);
	glUniform1f(glGetUniformLocation(depth_prog, "ice"), ice);
	glUniform1f(glGetUniformLocation(depth_prog, "d_ice"), d_ice);
	glUniform1f(glGetUniformLocation(depth_prog, "d_t"), d_t);
	glUniform1f(glGetUniformLocation(depth_prog, "seed"),
	    crc64_rand_fract());
	glutils_draw_quads(&priv->quads, depth_prog);

	glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER, priv->norm_fbo);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glUseProgram(norm_prog);
	glBindTexture(GL_TEXTURE_2D, priv->depth_tex[priv->cur]);
	glUniform1i(glGetUniformLocation(norm_prog, "depth"), 0);
	glUniformMatrix4fv(glGetUniformLocation(norm_prog, "pvm"), 1,
	    GL_FALSE, (void *)priv->pvm);
	glutils_draw_quads(&priv->quads, norm_prog);

	glViewport(vp[0], vp[1], vp[2], vp[3]);
	glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER, old_fbo);

	librain_reset_clip_control();
	glEnable(GL_CULL_FACE);
	glEnable(GL_DEPTH_TEST);

	priv->prev_ice = ice;
	priv->cur = !priv->cur;
	priv->prev_render_t = cur_sim_t;
}

static void
render_blur_tex(surf_ice_t *surf, int i, GLuint tex, double blur_radius)
{
	surf_ice_impl_t *priv = surf->priv;

	glBindFramebufferEXT(GL_FRAMEBUFFER, priv->blur_fbo[i]);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);
	glUniform1i(glGetUniformLocation(blur_prog, "tex"), 0);
	glUniform1f(glGetUniformLocation(blur_prog, "rand_seed"),
	    dr_getf(&drs.sim_time));
	glUniform1f(glGetUniformLocation(blur_prog, "blur_radius"),
	    blur_radius);
	glUniformMatrix4fv(glGetUniformLocation(blur_prog, "pvm"), 1,
	    GL_FALSE, (void *)priv->pvm);
	glutils_draw_quads(&priv->quads, blur_prog);
}

static void
render_blur(surf_ice_t *surf, double blur_radius)
{
	surf_ice_impl_t *priv = surf->priv;
	GLint old_fbo;
	int vp[4];

	glutils_debug_push(0, "ice_render_blur(%s)", surf->name);

	/* motion blur is rarely used, so lazy alloc the objects */
	if (priv->blur_tex[0] == 0) {
		glGenTextures(2, priv->blur_tex);
		glGenFramebuffers(2, priv->blur_fbo);
		setup_texture(priv->blur_tex[0], GL_R32F,
		    surf->w, surf->h, GL_RED, GL_FLOAT, NULL);
		setup_texture(priv->blur_tex[1], GL_RG8,
		    surf->w, surf->h, GL_RG, GL_UNSIGNED_BYTE, NULL);
		for (int i = 0; i < 2; i++) {
			setup_color_fbo_for_tex(priv->blur_fbo[i],
			    priv->blur_tex[i], 0, 0, B_FALSE);
			IF_TEXSZ(TEXSZ_ALLOC_INSTANCE(librain_ice_blur_tex,
			    surf, NULL, 0, GL_RG, GL_UNSIGNED_BYTE,
			    surf->w, surf->h));
		}
	}

	old_fbo = librain_get_current_fbo();
	librain_get_current_vp(vp);
	glViewport(0, 0, surf->w, surf->h);

	glUseProgram(blur_prog);
	render_blur_tex(surf, 0, priv->depth_tex[priv->cur], blur_radius);
	render_blur_tex(surf, 1, priv->norm_tex, blur_radius);

	glViewport(vp[0], vp[1], vp[2], vp[3]);
	glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER, old_fbo);

	glutils_debug_pop();
}

static void
render_obj(surf_ice_t *surf, double blur_radius)
{
	surf_ice_impl_t *priv = surf->priv;
	mat4 pvm;

	if (blur_radius > 0)
		render_blur(surf, blur_radius);

	glutils_debug_push(0, "ice_render_obj(%s)", surf->name);

	glUseProgram(render_prog);

	glUniform1f(glGetUniformLocation(render_prog, "growth_mult"),
	    surf->growth_mult);
	glActiveTexture(GL_TEXTURE0);
	if (blur_radius > 0) {
		ASSERT(priv->blur_tex[0] != 0);
		glBindTexture(GL_TEXTURE_2D, priv->blur_tex[0]);
	} else {
		glBindTexture(GL_TEXTURE_2D, priv->depth_tex[priv->cur]);
	}
	glUniform1i(glGetUniformLocation(render_prog, "depth"), 0);

	glActiveTexture(GL_TEXTURE1);
	if (blur_radius > 0) {
		ASSERT(priv->blur_tex[1] != 0);
		glBindTexture(GL_TEXTURE_2D, priv->blur_tex[1]);
	} else {
		glBindTexture(GL_TEXTURE_2D, priv->norm_tex);
	}
	glUniform1i(glGetUniformLocation(render_prog, "norm"), 1);

	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, librain_get_screenshot_tex());
	glUniform1i(glGetUniformLocation(render_prog, "bg"), 2);

	glUniform3f(glGetUniformLocation(render_prog, "sun_dir"),
	    sun_dir[0], sun_dir[1], sun_dir[2]);
	glUniform1f(glGetUniformLocation(render_prog, "sun_pitch"),
	    dr_getf(&drs.sun_pitch));
	glUniformMatrix4fv(glGetUniformLocation(render_prog, "acf_orient"), 1,
	    GL_FALSE, (void *)acf_orient);

	librain_get_pvm(pvm);
	glDepthMask(GL_FALSE);
	glEnable(GL_BLEND);
	obj8_draw_group(priv->obj, priv->group_id, render_prog, pvm);
	glDepthMask(GL_TRUE);

	glUseProgram(0);
	glActiveTexture(GL_TEXTURE0);

	glutils_debug_pop();
}

static void
recompute_coverage(surf_ice_t *surf, GLfloat *ptr)
{
	surf_ice_impl_t *priv = surf->priv;
	double ice = 0;

	for (unsigned i = 0; i < surf->w * surf->h; i++) {
		if (ice < ptr[i])
			ice = ptr[i];
	}

	priv->act_ice = ice / (surf->w * surf->h);
}

static void
act_ice_state_resync(surf_ice_t *surf, double ice)
{
	surf_ice_impl_t *priv = surf->priv;

	if (ice == 0) {
		/* No need to sync at zero icing levels */
		if (priv->packbuf != 0) {
			IF_TEXSZ(TEXSZ_FREE_BYTES_INSTANCE(librain_ice_packbuf,
			    surf, surf->w * surf->h * sizeof (GLfloat)));
			glDeleteBuffers(1, &priv->packbuf);
			priv->packbuf_sync = NULL;
			priv->packbuf = 0;
		}
	} else {
		/* Create the packbuf if necessary */
		if (priv->packbuf == 0) {
			glGenBuffers(1, &priv->packbuf);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, priv->packbuf);
			glBufferData(GL_PIXEL_PACK_BUFFER,
			    surf->w * surf->h * sizeof (GLfloat), NULL,
			    GL_STREAM_READ);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

			IF_TEXSZ(TEXSZ_ALLOC_BYTES_INSTANCE(librain_ice_packbuf,
			    surf, NULL, 0, surf->w * surf->h *
			    sizeof (GLfloat)));
		}
		/* If we have a buffer in flight, is it time to grab it? */
		if (priv->packbuf_sync != NULL) {
			if (glClientWaitSync(priv->packbuf_sync, 0, 0) !=
			    GL_TIMEOUT_EXPIRED) {
				GLfloat *ptr;

				glBindBuffer(GL_PIXEL_PACK_BUFFER,
				    priv->packbuf);
				ptr = glMapBuffer(GL_PIXEL_PACK_BUFFER,
				    GL_READ_ONLY);
				if (ptr != NULL)
					recompute_coverage(surf, ptr);
				glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
				priv->packbuf_sync = NULL;
			}
		} else if (cur_real_t - priv->last_resync_t >
		    ICE_RESYNC_INTVAL) {
			glBindBuffer(GL_PIXEL_PACK_BUFFER, priv->packbuf);
			glReadPixels(0, 0, surf->w, surf->h, GL_RED, GL_FLOAT,
			    NULL);
			priv->packbuf_sync =
			    glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
			priv->last_resync_t = cur_real_t;
		}
	}
}

void
surf_ice_render(surf_ice_t *surf, double ice, bool_t deice_on,
    double blur_radius, bool_t visible)
{
	surf_ice_impl_t *priv = surf->priv;
	double d_t;
	double cur_sim_t = dr_getf(&drs.sim_time);

	ASSERT(priv != NULL);

	if (priv->prev_render_t == 0)
		priv->prev_render_t = cur_sim_t;
	d_t = cur_sim_t - priv->prev_render_t;

	if (ice > 0) {
		priv->last_ice_t = cur_sim_t;
		last_ice_real_t = cur_real_t;
	}

	if (d_t > 0.1)
		update_depth(surf, cur_sim_t, d_t, ice, deice_on);

	if (cur_sim_t - priv->last_ice_t > ICE_RENDER_MAX_DELAY) {
		/* Clear any remaining icing - should be nearly all gone */
		priv->prev_ice = 0;
		priv->act_ice = 0;
		return;
	}

	glutils_debug_push(0, "surf_ice_render(%s)", surf->name);

	act_ice_state_resync(surf, ice);

	if (visible)
		render_obj(surf, blur_radius);

	glutils_debug_pop();
}

void
surf_ice_clear(surf_ice_t *surf)
{
	surf_ice_impl_t *priv = surf->priv;
	GLfloat depth_buf[surf->w * surf->h];
	uint16_t norm_buf[surf->w * surf->h];

	ASSERT(priv != NULL);
	priv->prev_ice = 0;

	glutils_debug_push(0, "surf_ice_clear(%s)", surf->name);

	memset(depth_buf, 0, sizeof (depth_buf));
	memset(norm_buf, 0, sizeof (norm_buf));

	for (int i = 0; i < 2; i++) {
		glBindTexture(GL_TEXTURE_2D, priv->depth_tex[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, surf->w, surf->h, 0,
		    GL_RED, GL_FLOAT, depth_buf);
	}
	glBindTexture(GL_TEXTURE_2D, priv->norm_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RG, surf->w, surf->h, 0,
	    GL_RG, GL_UNSIGNED_BYTE, norm_buf);

	if (priv->blur_tex[0] != 0) {
		glBindTexture(GL_TEXTURE_2D, priv->blur_tex[0]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, surf->w, surf->h, 0,
		    GL_RED, GL_FLOAT, depth_buf);
		glBindTexture(GL_TEXTURE_2D, priv->blur_tex[1]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RG, surf->w, surf->h, 0,
		    GL_RG, GL_UNSIGNED_BYTE, norm_buf);
	}

	glutils_debug_pop();
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
	mat4 m1, m2;

	/* prepare sun light illumination parameters */
	sun_dir[0] = 0;
	sun_dir[1] = 0;
	sun_dir[2] = -1;
	glm_vec_rotate(sun_dir, DEG2RAD(dr_getf(&drs.sun_pitch)),
	    (vec3){1, 0, 0});
	glm_vec_rotate(sun_dir, DEG2RAD(dr_getf(&drs.sun_hdg)),
	    (vec3){0, -1, 0});

	glm_mat4_identity(m1);
	glm_rotate_y(m1, -DEG2RAD(dr_getf(&drs.hdg)), m2);
	glm_rotate_x(m2, DEG2RAD(dr_getf(&drs.pitch)), m1);
	glm_rotate_z(m1, -DEG2RAD(dr_getf(&drs.roll)), acf_orient);
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
