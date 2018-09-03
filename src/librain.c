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

#include <stddef.h>
#include <string.h>
#include <time.h>

#include <GL/glew.h>

#include <XPLMDisplay.h>
#include <XPLMGraphics.h>
#include <XPLMUtilities.h>

#include <acfutils/assert.h>
#include <acfutils/crc64.h>
#include <acfutils/dr.h>
#include <acfutils/glutils.h>
#include <acfutils/math.h>
#include <acfutils/perf.h>
#include <acfutils/png.h>
#include <acfutils/safe_alloc.h>
#include <acfutils/shader.h>
#include <acfutils/time.h>

#include "glpriv.h"
#include "librain.h"

#define	RAIN_DRAW_TIMEOUT	15	/* seconds */

#define	MIN_PRECIP_ICE_ADD	0.05	/* dimensionless */
#define	PRECIP_ICE_TEMP_THRESH	4	/* Celsius */

typedef enum {
	PANEL_RENDER_TYPE_2D = 0,
	PANEL_RENDER_TYPE_3D_UNLIT = 1,
	PANEL_RENDER_TYPE_3D_LIT = 2
} panel_render_type_t;

static bool_t	inited = B_FALSE;
static bool_t	debug_draw = B_FALSE;

static GLuint	screenshot_tex = 0;
static GLuint	screenshot_fbo = 0;
static GLint	old_vp[4] = { -1, -1, -1, -1 };
static GLint	new_vp[4] = { -1, -1, -1, -1 };
static float	last_rain_t = 0;

static GLint	z_depth_prog = 0;
static GLint	ws_temp_prog = 0;
static GLint	rain_stage1_prog = 0;
static GLint	rain_stage2_prog = 0;
static GLint	ws_rain_prog = 0;
static GLint	ws_smudge_prog = 0;

static GLint	saved_vp[4] = { -1, -1, -1, -1 };
static bool_t	prepare_ran = B_FALSE;
static double	prev_win_ice = 0;
static double	precip_intens = 0;
static float	last_run_t = 0;

static shader_info_t generic_vert_info = { .filename = "generic.vert.spv" };
static shader_info_t ws_temp_frag_info = { .filename = "ws_temp.frag.spv" };
static shader_info_t rain_stage1_frag_info =
    { .filename = "rain_stage1.frag.spv" };
static shader_info_t rain_stage2_frag_info =
    { .filename = "rain_stage2.frag.spv" };
static shader_info_t ws_rain_frag_info = { .filename = "ws_rain.frag.spv" };
static shader_info_t ws_smudge_frag_info = { .filename = "ws_smudge.frag.spv" };
static shader_info_t nil_frag_info = { .filename = "nil.frag.spv" };

static shader_prog_info_t ws_temp_prog_info = {
    .progname = "ws_temp",
    .vert = &generic_vert_info,
    .frag = &ws_temp_frag_info
};

static shader_prog_info_t rain_stage1_prog_info = {
    .progname = "rain_stage1",
    .vert = &generic_vert_info,
    .frag = &rain_stage1_frag_info
};

static shader_prog_info_t rain_stage2_prog_info = {
    .progname = "rain_stage2",
    .vert = &generic_vert_info,
    .frag = &rain_stage2_frag_info
};

static shader_prog_info_t ws_rain_prog_info = {
    .progname = "ws_rain",
    .vert = &generic_vert_info,
    .frag = &ws_rain_frag_info
};

static shader_prog_info_t ws_smudge_prog_info = {
    .progname = "ws_smudge",
    .vert = &generic_vert_info,
    .frag = &ws_smudge_frag_info,
};

static shader_prog_info_t z_depth_prog_info = {
    .progname = "z_depth",
    .vert = &generic_vert_info,
    .frag = &nil_frag_info,
    .attr_binds = default_vtx_attr_binds
};

static mat4 glob_pvm = GLM_MAT4_IDENTITY;
static mat4 water_depth_pvm = GLM_MAT4_IDENTITY;
static mat4 water_norm_pvm = GLM_MAT4_IDENTITY;
static mat4 ws_temp_pvm = GLM_MAT4_IDENTITY;

typedef struct {
	const librain_glass_t	*glass;

	float			last_stage1_t;

#define	WATER_DEPTH_TEX_W	1024
#define	WATER_DEPTH_TEX_H	1024
	int			water_depth_cur;
	GLuint			water_depth_tex[2];
	GLuint			water_depth_fbo[2];
	glutils_quads_t		water_depth_quads;

#define	WATER_NORM_TEX_W	1024
#define	WATER_NORM_TEX_H	1024
	GLuint			water_norm_tex;
	GLuint			water_norm_fbo;
	glutils_quads_t		water_norm_quads;

#define	WS_TEMP_TEX_W		256
#define	WS_TEMP_TEX_H		256
#define	WS_TEMP_COMP_INTVAL	(1.0 / 15.0)	/* 20 fps */
	float			last_ws_temp_t;
	GLuint			ws_temp_tex[2];
	GLuint			ws_temp_fbo[2];
	int			ws_temp_cur;
	glutils_quads_t		ws_temp_quads;

	GLuint			ws_smudge_tex;
	GLuint			ws_smudge_fbo;

	vect2_t			gp;
	vect2_t			tp;
	vect2_t			wp;
	float			thrust;
	float			wind;
} glass_info_t;

static glass_info_t *glass_infos = NULL;
static size_t num_glass_infos = 0;

static struct {
	dr_t	panel_render_type;
	dr_t	sim_time;
	dr_t	proj_matrix;
	dr_t	acf_matrix;
	dr_t	viewport;
	dr_t	precip_rat;
	dr_t	prop_thrust;
	dr_t	hdg;
	dr_t	beta;
	dr_t	gs;
	dr_t	wind_dir;
	dr_t	wind_spd;
	dr_t	rot_rate;
	dr_t	le_temp;
	dr_t	amb_temp;
	dr_t	window_ice;
	bool_t	VR_enabled_avail;
	dr_t	VR_enabled;
} drs;

#define	RAIN_COMP_PHASE		xplm_Phase_Gauges
#define	RAIN_COMP_BEFORE	0

static void
update_vectors(glass_info_t *gi)
{
	float rot_rate = dr_getf(&drs.rot_rate);
	double wind_spd = KT2MPS(dr_getf(&drs.wind_spd));
	double gs = dr_getf(&drs.gs);
	vect2_t wind_comp = VECT2(0, wind_spd);
	vect2_t gs_comp = VECT2(0, gs);
	vect2_t total_wind;
	vect2_t thrust_v;

	gi->gp = vect2_scmul(gi->glass->gravity_point, WATER_DEPTH_TEX_W);
	gi->tp = vect2_scmul(gi->glass->thrust_point, WATER_DEPTH_TEX_W);
	gi->wp = vect2_scmul(gi->glass->wind_point, WATER_DEPTH_TEX_W);

	wind_comp = vect2_rot(wind_comp,
	    normalize_hdg(dr_getf(&drs.wind_dir)) - dr_getf(&drs.hdg));
	wind_comp = vect2_scmul(wind_comp, gi->glass->wind_factor);

	gs_comp = vect2_rot(gs_comp, dr_getf(&drs.beta));
	total_wind = vect2_add(wind_comp, gs_comp);

	total_wind = vect2_scmul(total_wind, 1.0 / gi->glass->max_tas);

	thrust_v = vect2_scmul(VECT2(0,
	    MAX(dr_getf(&drs.prop_thrust) / gi->glass->max_thrust, 0)),
	    gi->glass->thrust_factor);

	gi->wind = clamp(vect2_abs(total_wind), 0, 1);

	gi->thrust = clamp(vect2_abs(thrust_v), 0, 1);
	if (vect2_abs(total_wind) > 0.001)
		gi->wp = vect2_rot(gi->wp, dir2hdg(total_wind));

	gi->gp = vect2_rot(gi->gp, clamp(-2 * rot_rate, -30, 30));
}

static void
update_viewport(void)
{
	GLint vp[4];

	glGetIntegerv(GL_VIEWPORT, vp);
	if (old_vp[2] == vp[2] && old_vp[3] == vp[3])
		return;

	/* If the viewport size has changed, update the textures. */
	memcpy(old_vp, vp, sizeof (vp));
	XPLMBindTexture2d(screenshot_tex, GL_TEXTURE_2D);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, vp[2], vp[3], 0,
	    GL_RGB, GL_UNSIGNED_BYTE, NULL);

	for (size_t i = 0; i < num_glass_infos; i++) {
		XPLMBindTexture2d(glass_infos[i].ws_smudge_tex, GL_TEXTURE_2D);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vp[2], vp[3], 0,
		    GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	}
}

void
librain_refresh_screenshot(void)
{
	GLint old_fbo;

	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_fbo);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, old_fbo);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, screenshot_fbo);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glBlitFramebuffer(0, 0, old_vp[2], old_vp[3],
	    0, 0, old_vp[2], old_vp[3], GL_COLOR_BUFFER_BIT, GL_NEAREST);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_fbo);
}

void
librain_draw_z_depth(obj8_t *obj, const char **z_depth_group_ids)
{
	if (!prepare_ran)
		return;

	if (!debug_draw)
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glUseProgram(z_depth_prog);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	if (z_depth_group_ids != NULL) {
		for (int i = 0; z_depth_group_ids[i] != NULL; i++)
			obj8_draw_group(obj, z_depth_group_ids[i],
			    z_depth_prog, glob_pvm);
	} else {
		obj8_draw_group(obj, NULL, z_depth_prog, glob_pvm);
	}
	glUseProgram(0);
	if (!debug_draw)
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

static void
ws_temp_comp(glass_info_t *gi)
{
	float now = dr_getf(&drs.sim_time);
	float d_t = now - gi->last_ws_temp_t;
	float rand_seed = (crc64_rand() % 1000000) / 1000000.0;
	GLint old_fbo;

	if (d_t < WS_TEMP_COMP_INTVAL)
		return;

	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_fbo);

	XPLMSetGraphicsState(0, 1, 0, 1, 1, 1, 1);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
	    gi->ws_temp_fbo[!gi->ws_temp_cur]);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);

	glUseProgram(ws_temp_prog);

	glUniformMatrix4fv(glGetUniformLocation(ws_temp_prog, "pvm"),
	    1, GL_FALSE, (void *)ws_temp_pvm);

	glActiveTexture(GL_TEXTURE0);
	XPLMBindTexture2d(gi->ws_temp_tex[gi->ws_temp_cur], GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(ws_temp_prog, "src"), 0);

	glActiveTexture(GL_TEXTURE1);
	XPLMBindTexture2d(gi->water_depth_tex[gi->water_depth_cur],
	    GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(ws_temp_prog, "depth"), 1);

	glUniform1f(glGetUniformLocation(ws_temp_prog, "rand_seed"),
	    rand_seed);
	glUniform1f(glGetUniformLocation(ws_temp_prog, "le_temp"),
	    C2KELVIN(dr_getf(&drs.le_temp)));
	glUniform1f(glGetUniformLocation(ws_temp_prog, "cabin_temp"),
	    gi->glass->cabin_temp);
	glUniform1f(glGetUniformLocation(ws_temp_prog, "wind_fact"),
	    MAX(gi->wind, gi->thrust));
	glUniform1f(glGetUniformLocation(ws_temp_prog, "d_t"), d_t);
	glUniform1f(glGetUniformLocation(ws_temp_prog, "inertia_in"),
	    gi->glass->therm_inertia);
	glUniform4fv(glGetUniformLocation(ws_temp_prog, "heat_zones"),
	    16, gi->glass->heat_zones);
	glUniform1fv(glGetUniformLocation(ws_temp_prog, "heat_tgt_temps"),
	    4, gi->glass->heat_tgt_temps);
	glUniform1f(glGetUniformLocation(ws_temp_prog, "precip_intens"),
	    precip_intens * gi->glass->slant_factor);
	glUniform2fv(glGetUniformLocation(ws_temp_prog, "hot_air_src"),
	    4, gi->glass->hot_air_src);
	glUniform1fv(glGetUniformLocation(ws_temp_prog, "hot_air_radius"),
	    2, gi->glass->hot_air_radius);
	glUniform1fv(glGetUniformLocation(ws_temp_prog, "hot_air_temp"),
	    2, gi->glass->hot_air_temp);

	glutils_draw_quads(&gi->ws_temp_quads, ws_temp_prog);

	glUseProgram(0);
	glActiveTexture(GL_TEXTURE0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_fbo);

	gi->ws_temp_cur = !gi->ws_temp_cur;
	gi->last_ws_temp_t = now;
}

static void
rain_stage1_comp(glass_info_t *gi)
{
	GLint old_fbo;
	float rand_seed = (crc64_rand() % 1000000) / 1000000.0;
	float cur_t = dr_getf(&drs.sim_time);
	float d_t = cur_t - gi->last_stage1_t;

	if (gi->last_stage1_t == 0.0)
		gi->last_stage1_t = cur_t;
	d_t = cur_t - gi->last_stage1_t;

	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_fbo);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
	    gi->water_depth_fbo[!gi->water_depth_cur]);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(rain_stage1_prog);

	glUniformMatrix4fv(glGetUniformLocation(rain_stage1_prog, "pvm"),
	    1, GL_FALSE, (void *)water_depth_pvm);

	glActiveTexture(GL_TEXTURE0);
	XPLMBindTexture2d(gi->water_depth_tex[gi->water_depth_cur],
	    GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(rain_stage1_prog, "tex"), 0);

	glActiveTexture(GL_TEXTURE1);
	XPLMBindTexture2d(gi->ws_temp_tex[gi->ws_temp_cur], GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(rain_stage1_prog, "temp_tex"), 1);

	glUniform1f(glGetUniformLocation(rain_stage1_prog, "rand_seed"),
	    rand_seed);
	glUniform1f(glGetUniformLocation(rain_stage1_prog, "precip_intens"),
	    precip_intens * gi->glass->slant_factor);
	glUniform1f(glGetUniformLocation(rain_stage1_prog, "thrust"),
	    gi->thrust);
	glUniform1f(glGetUniformLocation(rain_stage1_prog, "wind"), gi->wind);
	glUniform1f(glGetUniformLocation(rain_stage1_prog, "gravity"),
	    (float)gi->glass->gravity_factor);
	glUniform1f(glGetUniformLocation(rain_stage1_prog, "d_t"), d_t);
	glUniform2f(glGetUniformLocation(rain_stage1_prog, "gp"),
	    gi->gp.x, gi->gp.y);
	glUniform2f(glGetUniformLocation(rain_stage1_prog, "tp"),
	    gi->tp.x, gi->tp.y);
	glUniform2f(glGetUniformLocation(rain_stage1_prog, "wp"),
	    gi->wp.x, gi->wp.y);
	glUniform1f(glGetUniformLocation(rain_stage1_prog, "wind_temp"),
	    C2KELVIN(dr_getf(&drs.amb_temp)));

	glutils_draw_quads(&gi->water_depth_quads, rain_stage1_prog);

	glUseProgram(0);
	glActiveTexture(GL_TEXTURE0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_fbo);

	gi->last_stage1_t = cur_t;
}

static void
rain_stage2_comp(glass_info_t *gi)
{
	GLint old_fbo;

	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_fbo);

	XPLMSetGraphicsState(0, 1, 0, 1, 1, 1, 1);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gi->water_norm_fbo);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(rain_stage2_prog);

	glUniformMatrix4fv(glGetUniformLocation(rain_stage2_prog, "pvm"),
	    1, GL_FALSE, (void *)water_norm_pvm);

	glActiveTexture(GL_TEXTURE0);
	XPLMBindTexture2d(gi->water_depth_tex[!gi->water_depth_cur],
	    GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(rain_stage2_prog, "tex"), 0);

	glActiveTexture(GL_TEXTURE1);
	XPLMBindTexture2d(gi->ws_temp_tex[gi->ws_temp_cur], GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(rain_stage2_prog, "temp_tex"), 1);

	glUniform2f(glGetUniformLocation(rain_stage2_prog, "my_tex_sz"),
	    WATER_NORM_TEX_W, WATER_NORM_TEX_H);
	glUniform2f(glGetUniformLocation(rain_stage2_prog, "tp"),
	    gi->tp.x, gi->tp.y);
	glUniform1f(glGetUniformLocation(rain_stage2_prog, "thrust"),
	    gi->thrust);
	glUniform2f(glGetUniformLocation(rain_stage2_prog, "wp"),
	    gi->wp.x, gi->wp.y);
	glUniform1f(glGetUniformLocation(rain_stage2_prog, "wind"), gi->wind);
	glUniform1f(glGetUniformLocation(rain_stage2_prog, "precip_intens"),
	    precip_intens * gi->glass->slant_factor);
	glUniform1f(glGetUniformLocation(rain_stage2_prog, "window_ice"),
	    dr_getf(&drs.window_ice));

	glutils_draw_quads(&gi->water_norm_quads, rain_stage2_prog);

	glUseProgram(0);
	glActiveTexture(GL_TEXTURE0);
	XPLMBindTexture2d(gi->water_norm_tex, GL_TEXTURE_2D);
	glGenerateMipmap(GL_TEXTURE_2D);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_fbo);
}

int
rain_comp_cb(XPLMDrawingPhase phase, int before, void *refcon)
{
	UNUSED(phase);
	UNUSED(before);
	UNUSED(refcon);

	/* Make sure we only run the computations once per frame */
	if (dr_geti(&drs.panel_render_type) != PANEL_RENDER_TYPE_3D_UNLIT)
		return (1);

	glutils_disable_all_client_state();
	for (size_t i = 0; i < num_glass_infos; i++) {
		glass_info_t *gi = &glass_infos[i];

		update_vectors(gi);
		ws_temp_comp(gi);
		rain_stage1_comp(gi);
		rain_stage2_comp(gi);
		gi->water_depth_cur = !gi->water_depth_cur;
	}

	return (1);
}

static void
draw_ws_effects(glass_info_t *gi)
{
	GLint old_fbo;

	glutils_disable_all_client_state();

	/*
	 * Pre-stage: render the actual image to a side buffer, but without
	 * any smudging.
	 */
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gi->ws_smudge_fbo);
	glClear(GL_COLOR_BUFFER_BIT);

	XPLMSetGraphicsState(0, 1, 0, 1, 1, 1, 1);

	glUseProgram(ws_rain_prog);

	glActiveTexture(GL_TEXTURE0);
	XPLMBindTexture2d(gi->water_norm_tex, GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(ws_rain_prog, "norm_tex"), 0);

	glActiveTexture(GL_TEXTURE1);
	XPLMBindTexture2d(screenshot_tex, GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(ws_rain_prog, "screenshot_tex"), 1);

	glActiveTexture(GL_TEXTURE2);
	XPLMBindTexture2d(gi->water_depth_tex[!gi->water_depth_cur],
	    GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(ws_rain_prog, "depth_tex"), 2);

	glUniform4f(glGetUniformLocation(ws_rain_prog, "vp"),
	    new_vp[0], new_vp[1], new_vp[2], new_vp[3]);

	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);

	if (gi->glass->group_ids != NULL) {
		for (int i = 0; gi->glass->group_ids[i] != NULL; i++) {
			obj8_draw_group(gi->glass->obj, gi->glass->group_ids[i],
			    ws_rain_prog, glob_pvm);
		}
	} else {
		obj8_draw_group(gi->glass->obj, NULL, ws_rain_prog, glob_pvm);
	}

	/*
	 * Final stage: render the prepped displaced texture and apply
	 * variable smudging based on water depth.
	 */
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_fbo);

	glUseProgram(ws_smudge_prog);

	glActiveTexture(GL_TEXTURE0);
	XPLMBindTexture2d(screenshot_tex, GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(ws_smudge_prog, "screenshot_tex"), 0);

	glActiveTexture(GL_TEXTURE1);
	XPLMBindTexture2d(gi->ws_smudge_tex, GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(ws_smudge_prog, "ws_tex"), 1);

	glActiveTexture(GL_TEXTURE2);
	XPLMBindTexture2d(gi->water_depth_tex[!gi->water_depth_cur],
	    GL_TEXTURE_2D);
	glUniform1i(glGetUniformLocation(ws_smudge_prog, "depth_tex"), 2);

	glUniform4f(glGetUniformLocation(ws_smudge_prog, "vp"),
	    new_vp[0], new_vp[1], new_vp[2], new_vp[3]);

	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	if (gi->glass->group_ids != NULL) {
		for (int i = 0; gi->glass->group_ids[i] != NULL; i++) {
			obj8_draw_group(gi->glass->obj, gi->glass->group_ids[i],
			    ws_smudge_prog, glob_pvm);
		}
	} else {
		obj8_draw_group(gi->glass->obj, NULL, ws_smudge_prog, glob_pvm);
	}

	glUseProgram(0);
	glActiveTexture(GL_TEXTURE0);
}

static void
compute_precip(double now)
{
	double d_t = now - last_run_t;
	double cur_win_ice;

	if (d_t <= 0)
		return;

	precip_intens = dr_getf(&drs.precip_rat);
	cur_win_ice = dr_getf(&drs.window_ice);

	/*
	 * If the window is icing up, but X-Plane thinks there's no precip
	 * to cause it, we attempt to derive a precip rate from the rate of
	 * window ice accumulation.
	 */
	if (precip_intens < MIN_PRECIP_ICE_ADD &&
	    dr_getf(&drs.amb_temp) < PRECIP_ICE_TEMP_THRESH &&
	    cur_win_ice > 0) {
		double d_ice = (cur_win_ice - prev_win_ice) / d_t;
		double d_precip;

		if (d_ice > 0)
			d_precip = wavg(0.025, 0.05, clamp(d_ice, 0, 1));
		else
			d_precip = wavg(0.02, 0, clamp(ABS(d_ice), 0, 1));
		precip_intens += d_precip;
	}

	prev_win_ice = cur_win_ice;
	last_run_t = now;
}

void
librain_draw_prepare(bool_t force)
{
	mat4 mv_matrix;
	mat4 proj_matrix;
	GLint vp[4];
	int w, h;
	double now = dr_getf(&drs.sim_time);

	compute_precip(now);

	if (precip_intens > 0 || dr_getf(&drs.amb_temp) <= 4)
		last_rain_t = now;

	/*
	 * FIXME: avoid running when we don't have ice on the
	 * windshield, even if the outside air temp is below zero.
	 */
	if (now - last_rain_t > RAIN_DRAW_TIMEOUT && !force &&
	    dr_getf(&drs.amb_temp) > 4) {
		prepare_ran = B_FALSE;
		return;
	}
	prepare_ran = B_TRUE;

	update_viewport();
	librain_refresh_screenshot();

	glGetIntegerv(GL_VIEWPORT, vp);
	XPLMGetScreenSize(&w, &h);
	/*
	 * In VR the viewport width needs to be halved, because the viewport
	 * is split between the left and right eye.
	 */
	if (drs.VR_enabled_avail && dr_geti(&drs.VR_enabled) != 0)
		w /= 2;
	if (vp[2] != w || vp[3] != h) {
		memcpy(saved_vp, vp, sizeof (saved_vp));
		glViewport(vp[0], vp[1], w, h);
		new_vp[0] = vp[0];
		new_vp[1] = vp[1];
		new_vp[2] = w;
		new_vp[3] = h;
	} else {
		memcpy(new_vp, old_vp, sizeof (new_vp));
	}

	VERIFY3S(dr_getvf32(&drs.acf_matrix, (void *)mv_matrix, 0, 16),
	    ==, 16);
	VERIFY3S(dr_getvf32(&drs.proj_matrix, (void *)proj_matrix, 0, 16),
	    ==, 16);

	for (int i = 0; i < 4; i++)
		proj_matrix[3][i] /= 100;

	glm_mat4_mul(proj_matrix, mv_matrix, glob_pvm);
}

void
librain_draw_exec(void)
{
	if (dr_getf(&drs.sim_time) - last_rain_t <= RAIN_DRAW_TIMEOUT) {
		for (size_t i = 0; i < num_glass_infos; i++)
			draw_ws_effects(&glass_infos[i]);
	}
}

void
librain_draw_finish(void)
{
	if (!prepare_ran)
		return;

	if (saved_vp[0] != -1) {
		glViewport(saved_vp[0], saved_vp[1], saved_vp[2], saved_vp[3]);
		saved_vp[0] = saved_vp[1] = saved_vp[2] = saved_vp[3] = -1;
	}
}

static void
glass_info_init(glass_info_t *gi, const librain_glass_t *glass)
{
	GLfloat temp_tex[WS_TEMP_TEX_W * WS_TEMP_TEX_H];
	vect2_t ws_temp_pos[] = {
	    VECT2(0, 0), VECT2(0, WS_TEMP_TEX_H),
	    VECT2(WS_TEMP_TEX_W, WS_TEMP_TEX_H), VECT2(WS_TEMP_TEX_W, 0)
	};
	vect2_t water_depth_pos[] = {
	    VECT2(0, 0), VECT2(0, WATER_DEPTH_TEX_H),
	    VECT2(WATER_DEPTH_TEX_W, WATER_DEPTH_TEX_H),
	    VECT2(WATER_DEPTH_TEX_W, 0)
	};
	vect2_t water_norm_pos[] = {
	    VECT2(0, 0), VECT2(0, WATER_NORM_TEX_W),
	    VECT2(WATER_NORM_TEX_W, WATER_NORM_TEX_H),
	    VECT2(WATER_NORM_TEX_W, 0)
	};

	gi->glass = glass;

	/*
	 * Pre-stage: glass heating/cooling simulation.
	 */
	glGenTextures(2, gi->ws_temp_tex);
	glGenFramebuffers(2, gi->ws_temp_fbo);
	memset(temp_tex, 0, sizeof (temp_tex));
	for (int i = 0; i < 2; i++) {
		setup_texture(gi->ws_temp_tex[i], GL_R32F, WS_TEMP_TEX_W,
		    WS_TEMP_TEX_H, GL_RED, GL_FLOAT, temp_tex);
		setup_color_fbo_for_tex(gi->ws_temp_fbo[i],
		    gi->ws_temp_tex[i]);
	}
	glutils_init_2D_quads(&gi->ws_temp_quads, ws_temp_pos, NULL, 4);

	/*
	 * Stage 1: computing water depth.
	 */
	glGenTextures(2, gi->water_depth_tex);
	glGenFramebuffers(2, gi->water_depth_fbo);
	for (int i = 0; i < 2; i++) {
		setup_texture(gi->water_depth_tex[i], GL_RED,
		    WATER_DEPTH_TEX_W, WATER_DEPTH_TEX_H, GL_RED,
		    GL_FLOAT, NULL);
		setup_color_fbo_for_tex(gi->water_depth_fbo[i],
		    gi->water_depth_tex[i]);
	}
	glutils_init_2D_quads(&gi->water_depth_quads, water_depth_pos, NULL, 4);

	/*
	 * Stage 2: computing normals.
	 */
	glGenTextures(1, &gi->water_norm_tex);
	glGenFramebuffers(1, &gi->water_norm_fbo);

	setup_texture_filter(gi->water_norm_tex, GL_RG,
	    WATER_NORM_TEX_W, WATER_NORM_TEX_H, GL_RG, GL_UNSIGNED_BYTE,
	    NULL, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR);
	setup_color_fbo_for_tex(gi->water_norm_fbo, gi->water_norm_tex);
	glutils_init_2D_quads(&gi->water_norm_quads, water_norm_pos, NULL, 4);

	/*
	 * Final render pre-stage: capture the full res displacement,
	 * which is then used as the final smudge pass.
	 */
	glGenTextures(1, &gi->ws_smudge_tex);
	glGenFramebuffers(1, &gi->ws_smudge_fbo);
	setup_texture(gi->ws_smudge_tex, GL_RGBA, 16, 16, GL_RGBA,
	    GL_UNSIGNED_BYTE, NULL);
	setup_color_fbo_for_tex(gi->ws_smudge_fbo, gi->ws_smudge_tex);
}

static void
water_effects_init(const librain_glass_t *glass, size_t num)
{
	glass_infos = safe_calloc(sizeof (*glass_infos), num);
	num_glass_infos = num;

	for (size_t i = 0; i < num_glass_infos; i++)
		glass_info_init(&glass_infos[i], &glass[i]);
}

static void
water_effects_fini(void)
{
	DESTROY_OP(ws_temp_prog, 0, glDeleteProgram(ws_temp_prog));
	DESTROY_OP(rain_stage1_prog, 0, glDeleteProgram(rain_stage1_prog));
	DESTROY_OP(rain_stage2_prog, 0, glDeleteProgram(rain_stage2_prog));
	DESTROY_OP(ws_rain_prog, 0, glDeleteProgram(ws_rain_prog));
	DESTROY_OP(ws_smudge_prog, 0, glDeleteProgram(ws_smudge_prog));
	DESTROY_OP(z_depth_prog, 0, glDeleteProgram(z_depth_prog));

	for (size_t i = 0; i < num_glass_infos; i++) {
		glass_info_t *gi = &glass_infos[i];

		DESTROY_OP(gi->ws_temp_fbo[0], 0,
		    glDeleteFramebuffers(2, gi->ws_temp_fbo));
		DESTROY_OP(gi->ws_temp_tex[0], 0,
		    glDeleteTextures(2, gi->ws_temp_tex));
		glutils_destroy_quads(&gi->ws_temp_quads);

		DESTROY_OP(gi->water_depth_fbo[0], 0,
		    glDeleteFramebuffers(2, gi->water_depth_fbo));
		DESTROY_OP(gi->water_depth_tex[0], 0,
		    glDeleteTextures(2, gi->water_depth_tex));
		glutils_destroy_quads(&gi->water_depth_quads);

		DESTROY_OP(gi->water_norm_fbo, 0,
		    glDeleteFramebuffers(1, &gi->water_norm_fbo));
		DESTROY_OP(gi->water_norm_tex, 0,
		    glDeleteTextures(1, &gi->water_norm_tex));
		glutils_destroy_quads(&gi->water_norm_quads);

		DESTROY_OP(gi->ws_smudge_fbo, 0,
		    glDeleteFramebuffers(1, &gi->ws_smudge_fbo));
		DESTROY_OP(gi->ws_smudge_tex, 0,
		    glDeleteTextures(1, &gi->ws_smudge_tex));
	}

	free(glass_infos);
	glass_infos = NULL;
	num_glass_infos = 0;
}

bool_t
librain_reload_gl_progs(void)
{
	return (reload_gl_prog(&ws_temp_prog, &ws_temp_prog_info) &&
	    reload_gl_prog(&rain_stage1_prog, &rain_stage1_prog_info) &&
	    reload_gl_prog(&rain_stage2_prog, &rain_stage2_prog_info) &&
	    reload_gl_prog(&ws_rain_prog, &ws_rain_prog_info) &&
	    reload_gl_prog(&ws_smudge_prog, &ws_smudge_prog_info) &&
	    reload_gl_prog(&z_depth_prog, &z_depth_prog_info));
}

bool_t
librain_glob_init(void)
{
#ifdef	DLLMODE
	static bool_t glob_inited = B_FALSE;
	GLenum err;

	if (glob_inited)
		return (B_TRUE);

	log_init(XPLMDebugString, "librain");
	crc64_init();
	crc64_srand(microclock() + clock());

	/* GLEW bootstrap */
	err = glewInit();
	if (err != GLEW_OK) {
		/* Problem: glewInit failed, something is seriously wrong. */
		logMsg("FATAL ERROR: cannot initialize libGLEW: %s",
		    glewGetErrorString(err));
		return (B_FALSE);
	}
	if (!GLEW_VERSION_2_1) {
		logMsg("FATAL ERROR: your system doesn't support at "
		    "least OpenGL 2.1");
		return (B_FALSE);
	}

	glob_inited = B_TRUE;
#endif	/* DLLMODE */
	return (B_TRUE);
}

bool_t
librain_init(const char *the_shaderpath, const librain_glass_t *glass,
    size_t num)
{
	ASSERT(!inited);
	inited = B_TRUE;

	shaderpath = strdup(the_shaderpath);

	memset(&drs, 0, sizeof (drs));

	if (!librain_glob_init())
		return (B_FALSE);

	fdr_find(&drs.panel_render_type, "sim/graphics/view/panel_render_type");
	fdr_find(&drs.sim_time, "sim/time/total_running_time_sec");
	fdr_find(&drs.proj_matrix, "sim/graphics/view/projection_matrix");
	fdr_find(&drs.acf_matrix, "sim/graphics/view/acf_matrix");
	fdr_find(&drs.viewport, "sim/graphics/view/viewport");
	fdr_find(&drs.precip_rat,
	    "sim/weather/precipitation_on_aircraft_ratio");
	fdr_find(&drs.prop_thrust, "sim/flightmodel/engine/POINT_thrust");
	fdr_find(&drs.hdg, "sim/flightmodel/position/psi");
	fdr_find(&drs.beta, "sim/flightmodel/position/beta");
	fdr_find(&drs.gs, "sim/flightmodel/position/groundspeed");
	fdr_find(&drs.wind_dir, "sim/weather/wind_direction_degt");
	fdr_find(&drs.wind_spd, "sim/weather/wind_speed_kt");
	fdr_find(&drs.rot_rate, "sim/flightmodel/position/R");
	fdr_find(&drs.amb_temp, "sim/weather/temperature_ambient_c");
	fdr_find(&drs.le_temp, "sim/weather/temperature_le_c");
	fdr_find(&drs.window_ice, "sim/flightmodel/failures/window_ice");
	if (dr_find(&drs.VR_enabled, "sim/graphics/VR/enabled"))
		drs.VR_enabled_avail = B_TRUE;

	XPLMRegisterDrawCallback(rain_comp_cb, RAIN_COMP_PHASE,
	    RAIN_COMP_BEFORE, NULL);

	if (!GLEW_ARB_framebuffer_object) {
		logMsg("Cannot initialize: your OpenGL version doesn't appear "
		    "to support ARB_framebuffer_object");
		goto errout;
	}

	glGenTextures(1, &screenshot_tex);
	XPLMBindTexture2d(screenshot_tex, GL_TEXTURE_2D);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 10, 10, 0, GL_RGB,
	    GL_UNSIGNED_BYTE, NULL);

	glGenFramebuffers(1, &screenshot_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, screenshot_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_TEXTURE_2D, screenshot_tex, 0);
	VERIFY3U(glCheckFramebufferStatus(GL_FRAMEBUFFER), ==,
	    GL_FRAMEBUFFER_COMPLETE);

	water_effects_init(glass, num);

	if (!librain_reload_gl_progs())
		goto errout;

	glm_ortho(0, WATER_DEPTH_TEX_W, 0, WATER_DEPTH_TEX_H,
	    0, 1, water_depth_pvm);
	glm_ortho(0, WATER_NORM_TEX_W, 0, WATER_NORM_TEX_H,
	    0, 1, water_norm_pvm);
	glm_ortho(0, WS_TEMP_TEX_W, 0, WS_TEMP_TEX_H, 0, 1, ws_temp_pvm);

	return (B_TRUE);
errout:
	librain_fini();
	return (B_FALSE);
}

void
librain_fini(void)
{
	if (!inited)
		return;

	free(shaderpath);
	shaderpath = NULL;

	XPLMUnregisterDrawCallback(rain_comp_cb, RAIN_COMP_PHASE,
	    RAIN_COMP_BEFORE, NULL);

	DESTROY_OP(screenshot_fbo, 0, glDeleteFramebuffers(1, &screenshot_fbo));
	DESTROY_OP(screenshot_tex, 0, glDeleteTextures(1, &screenshot_tex));

	water_effects_fini();

	for (int i = 0; i < 4; i++) {
		old_vp[i] = -1;
		new_vp[i] = -1;
		saved_vp[i] = -1;
	}

	inited = B_FALSE;
}

void
librain_get_pvm(mat4 pvm)
{
	memcpy(pvm, glob_pvm, sizeof (mat4));
}

GLuint
librain_get_screenshot_tex(void)
{
	return (screenshot_tex);
}

/*
 * By setting this flag to true, any object you draw using
 * librain_draw_z_depth will be visible on the screen (instead of only
 * being used in a z-depth pass). Useful for debugging z-depth object
 * placement. The UV mapping (if any) of the geometry will be drawn
 * using a mixture of RGB colors, to give the objects some more shape.
 */
void
librain_set_debug_draw(bool_t flag)
{
	debug_draw = flag;
}
