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

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include <XPLMDisplay.h>
#include <XPLMGraphics.h>
#include <XPLMUtilities.h>

#include <acfutils/assert.h>
#include <acfutils/crc64.h>
#include <acfutils/dr.h>
#include <acfutils/glew.h>
#include <acfutils/glutils.h>
#include <acfutils/math.h>
#include <acfutils/perf.h>
#include <acfutils/png.h>
#include <acfutils/safe_alloc.h>
#include <acfutils/shader.h>
#include <acfutils/time.h>

#include "glpriv.h"
#include "librain.h"
#include "../shaders/droplets_data.h"

#define	RAIN_DRAW_TIMEOUT	120	/* seconds */
#define	DFL_VP_SIZE		16	/* pixels */

#define	MIN_PRECIP_ICE_ADD	0.05	/* dimensionless */
#define	PRECIP_ICE_TEMP_THRESH	4	/* Celsius */
#define	WIPER_SMEAR_DELAY	0.5	/* secs */

#define	DEPTH_TEX_SZ(gi)	((gi)->qual.use_compute ? 2048 : 1024)
#define	NORM_TEX_SZ(gi)		((gi)->qual.use_compute ? 2048 : 1024)
#define	GLASS_NAME(gi)	\
	(gi->glass->name == NULL ? "<unnamed>" : gi->glass->name)
#define	MIPLEVELS		8

TEXSZ_MK_TOKEN(librain_water_depth_tex);
TEXSZ_MK_TOKEN(librain_water_norm_tex);
TEXSZ_MK_TOKEN(librain_ws_temp_tex);
TEXSZ_MK_TOKEN(librain_ws_smudge_tex);
TEXSZ_MK_TOKEN(librain_screenshot_tex);

typedef enum {
	PANEL_RENDER_TYPE_2D = 0,
	PANEL_RENDER_TYPE_3D_UNLIT = 1,
	PANEL_RENDER_TYPE_3D_LIT = 2
} panel_render_type_t;

/*
 * Values of sim/graphics/view/draw_call_type - this is important
 * when capturing graphics matrices & viewport to support VR.
 */
typedef enum {
	DRAW_CALL_NONE =	0,
	DRAW_CALL_MONO =	1,
	DRAW_CALL_DCT_STEREO =	2,
	DRAW_CALL_LEFT_EYE =	3,
	DRAW_CALL_RIGHT_EYE =	4
} draw_call_type_t;

/*
 * Values of sim/graphics/view/plane_render_type
 */
typedef enum {
	PLANE_RENDER_NONE =	0,
	PLANE_RENDER_SOLID =	1,
	PLANE_RENDER_BLEND =	2
} plane_render_type_t;

/*
 * Values of sim/graphics/view/world_render_type
 */
typedef enum {
	WORLD_RENDER_TYPE_NORM = 0,
	WORLD_RENDER_TYPE_REFLECT = 1,
	WORLD_RENDER_TYPE_INVALID = 6
} world_render_type_t;

static int			xp_ver, xplm_ver;
static XPLMHostApplicationID	host_id;

static bool_t		inited = B_FALSE;
static bool_t		debug_draw = B_FALSE;
static bool_t		wipers_visible = B_FALSE;

static GLuint	screenshot_tex = 0;
static GLuint	screenshot_fbo = 0;
static GLuint	ws_smudge_tex = 0;
static GLuint	ws_smudge_fbo = 0;
static GLint	cur_vp[4] = { -1, -1, DFL_VP_SIZE, DFL_VP_SIZE };
static GLint	saved_vp[4] = { -1, -1, -1, -1 };
static bool	cached_rev_float_z = false;
static bool	cached_rev_y = false;
static GLint	saved_clip_origin;
static GLint	saved_depth_mode;
static GLint	saved_depth_func;
static GLfloat	saved_depth_clear;
static GLint	saved_front_face;
static GLint	ss_texsz[2] = { DFL_VP_SIZE, DFL_VP_SIZE };
static float	last_rain_t = 0;
static bool_t	rain_enabled = B_TRUE;

static GLuint	priv_depth_tex = 0;
static GLuint	priv_depth_tex_w = 0;
static GLuint	priv_depth_tex_h = 0;
static bool	priv_depth_override = false;

static bool_t	prepare_ran = B_FALSE;
static double	precip_intens = 0;
static float	last_run_t = 0;

static GLint	z_depth_prog = 0;
static GLint	stencil_init_prog = 0;

/* Captured matrix info during capture_mtx */
static struct {
	mat4	proj_matrix;
	mat4	acf_matrix;
	vec4	viewport;
} mtx_info[2];
static unsigned num_mtx_info = 0;

typedef struct {
	GLint	pvm;
	GLint	src;
	GLint	depth;
	GLint	rand_seed;
	GLint	le_temp;
	GLint	cabin_temp;
	GLint	wind_fact;
	GLint	d_t;
	GLint	inertia_in;
	GLint	heat_zones;
	GLint	heat_tgt_temps;
	GLint	precip_intens;
	GLint	hot_air_src;
	GLint	hot_air_radius;
	GLint	hot_air_temp;
} ws_temp_prog_loc_t;
static GLint	ws_temp_prog = 0;
static ws_temp_prog_loc_t ws_temp_prog_loc;

static GLint	rain_stage1_prog = 0;
static GLint	rain_stage2_prog = 0;
static GLint	rain_stage2_comp_prog = 0;
static GLint	ws_rain_prog = 0;
static GLint	ws_rain_comp_prog = 0;
static GLint	ws_smudge_prog = 0;
static GLint	ws_smudge_comp_prog = 0;
static GLint	droplets_prog = 0;
static GLint	droplets_paint_prog = 0;
static GLint	tails_prog = 0;

static shader_info_t generic_vert_info = { .filename = "generic.vert.spv" };
static shader_info_t ws_temp_frag_info = { .filename = "ws_temp.frag.spv" };
static shader_info_t rain_stage1_frag_info =
    { .filename = "rain_stage1.frag.spv" };
static shader_info_t rain_stage2_frag_info =
    { .filename = "rain_stage2.frag.spv" };
static shader_info_t rain_stage2_comp_frag_info =
    { .filename = "rain_stage2_comp.frag.spv" };
static shader_info_t ws_rain_frag_info = { .filename = "ws_rain.frag.spv" };
static shader_info_t ws_rain_comp_frag_info =
    { .filename = "ws_rain_comp.frag.spv" };
static shader_info_t ws_smudge_frag_info =
    { .filename = "ws_smudge.frag.spv" };
static shader_info_t ws_smudge_comp_frag_info =
    { .filename = "ws_smudge_comp.frag.spv" };
static shader_info_t nil_frag_info = { .filename = "nil.frag.spv" };
static shader_info_t droplets_comp_info = { .filename = "droplets.comp.spv" };
static shader_info_t droplets_vert_info = { .filename = "droplets.vert.spv" };
static shader_info_t droplets_frag_info = { .filename = "droplets.frag.spv" };
static shader_info_t tails_vert_info = { .filename = "tails.vert.spv" };
static shader_info_t tails_frag_info = { .filename = "tails.frag.spv" };
static shader_info_t stencil_vert_info = { .filename = "stencil.vert.spv" };


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

static shader_prog_info_t rain_stage2_comp_prog_info = {
    .progname = "rain_stage2_comp",
    .vert = &generic_vert_info,
    .frag = &rain_stage2_comp_frag_info
};

static shader_prog_info_t ws_rain_prog_info = {
    .progname = "ws_rain",
    .vert = &generic_vert_info,
    .frag = &ws_rain_frag_info
};

static shader_prog_info_t ws_rain_comp_prog_info = {
    .progname = "ws_rain_comp",
    .vert = &generic_vert_info,
    .frag = &ws_rain_comp_frag_info
};

static shader_prog_info_t ws_smudge_prog_info = {
    .progname = "ws_smudge",
    .vert = &generic_vert_info,
    .frag = &ws_smudge_frag_info
};

static shader_prog_info_t ws_smudge_comp_prog_info = {
    .progname = "ws_smudge_comp",
    .vert = &generic_vert_info,
    .frag = &ws_smudge_comp_frag_info
};

static shader_prog_info_t z_depth_prog_info = {
    .progname = "z_depth",
    .vert = &generic_vert_info,
    .frag = &nil_frag_info,
    .attr_binds = default_vtx_attr_binds
};

static shader_prog_info_t stencil_init_prog_info = {
    .progname = "stencil_init",
    .vert = &stencil_vert_info,
    .frag = &nil_frag_info
};

static shader_prog_info_t droplets_prog_info = {
    .progname = "droplets_comp",
    .comp = &droplets_comp_info
};

static shader_prog_info_t droplets_paint_prog_info = {
    .progname = "droplets_paint",
    .vert = &droplets_vert_info,
    .frag = &droplets_frag_info
};

static shader_prog_info_t tails_prog_info = {
    .progname = "tails",
    .vert = &tails_vert_info,
    .frag = &tails_frag_info
};

typedef struct {
	GLint	pvm;
	GLint	tex;
	GLint	temp_tex;
	GLint	rand_seed;
	GLint	precip_intens;
	GLint	thrust;
	GLint	wind;
	GLint	gravity;
	GLint	d_t;
	GLint	gp;
	GLint	tp;
	GLint	wp;
	GLint	wind_temp;
} rain_stage1_loc_t;

typedef struct {
	GLint	pos;
	GLint	ctr;
	GLint	radius;
	GLint	size;
} droplets_paint_loc_t;

typedef struct {
	GLint	pos;
	GLint	quant;
} tails_paint_loc_t;

typedef struct {
	GLint	ws_temp_tex;
	GLint	cur_t;
	GLint	d_t;
	GLint	rand_seed;
	GLint	gravity_point;
	GLint	gravity_force;
	GLint	wind_point;
	GLint	wind_force;
	GLint	thrust_point;
	GLint	thrust_force;
	GLint	precip_intens;
	GLint	min_droplet_sz;
	GLint	le_temp;
} droplets_move_loc_t;

typedef struct {
	GLint	pvm;
	GLint	tex;
	GLint	temp_tex;
	GLint	tp;
	GLint	thrust;
	GLint	wp;
	GLint	wind;
	GLint	precip_intens;
	GLint	window_ice;
} stage2_loc_t;

static rain_stage1_loc_t	rain_stage1_loc = { 0 };
static droplets_paint_loc_t	droplets_paint_prog_loc = { 0 };
static tails_paint_loc_t	tails_prog_loc = { 0 };
static droplets_move_loc_t	droplets_move_loc = { 0 };
static stage2_loc_t		stage2_comp_loc = { 0 };
static stage2_loc_t		stage2_loc = { 0 };

static mat4 glob_proj_matrix = GLM_MAT4_IDENTITY;
static mat4 glob_acf_matrix = GLM_MAT4_IDENTITY;
static mat4 glob_pvm = GLM_MAT4_IDENTITY;
static vec4 glob_vp;
static unsigned glob_call_index = 0;

typedef struct {
	double	angle_now;		/* radians */
	double	angle_now_t;		/* secs */
	double	angle_prev;		/* radians */
	double	angle_edge;		/* radians */
	double	angle_edge_t;		/* secs */
	bool_t	dir;			/* moving clockwise? */
	double	angle_smear;		/* radians */
	double	angle_smear_t;		/* secs */
} wiper_t;

typedef struct {
	const librain_glass_t	*glass;

	float			last_stage1_t;
	/*
	 * This flag indicates whether stage1 has rendered something or not.
	 * If this flag is not set, this avoids the buffer swap on the legacy
	 * effect and avoids re-rendering the normals (since they couldn't
	 * have changed if the water movement pass never ran).
	 */
	bool_t			stage1_adv;

	int			water_depth_cur;
	GLuint			water_depth_tex[2];
	GLuint			water_depth_tex_stencil[2];
	GLuint			water_depth_fbo[2];
	glutils_quads_t		water_depth_quads;

	GLuint			water_norm_tex;
	GLuint			water_norm_fbo;
	glutils_quads_t		water_norm_quads;

#define	WS_TEMP_TEX_W		256
#define	WS_TEMP_TEX_H		256
#define	WS_TEMP_COMP_INTVAL	(1.0 / 15.0)	/* 15 fps */
	float			last_ws_temp_t;
	GLuint			ws_temp_tex[2];
	GLuint			ws_temp_fbo[2];
	int			ws_temp_cur;
	glutils_quads_t		ws_temp_quads;

	GLuint			droplets_ssbo;
	GLuint			vertices_vtx_buf;
	GLuint			vertices_idx_buf;
	GLuint			tails_vtx_buf;
	GLuint			tails_idx_buf;

	vect2_t			gp;
	vect2_t			tp;
	vect2_t			wp;
	float			thrust;
	float			wind;

	wiper_t			wipers[MAX_WIPERS];

	mat4			water_depth_pvm;
	mat4			water_norm_pvm;
	mat4			ws_temp_pvm;

	librain_qual_t		qual;

	struct {
		vect2_t		wp;
	} compute;
} glass_info_t;

static glass_info_t *glass_infos = NULL;
static size_t num_glass_infos = 0;

static struct {
	const librain_glass_t	*glass;
	size_t			num;
} reinit;

static struct {
	dr_t	panel_render_type;
	dr_t	plane_render_type;
	dr_t	world_render_type;
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
	dr_t	draw_call_type;
	bool_t	rev_float_z_avail;
	dr_t	rev_float_z;
	bool_t	rev_y_avail;
	dr_t	rev_y;
	bool_t	aa_ratio_avail;
	dr_t	fsaa_ratio_x;
	dr_t	fsaa_ratio_y;
	bool	current_gl_fbo_avail;
	dr_t	current_gl_fbo;

	bool_t	xe_present;
	dr_t	xe_active;
	dr_t	xe_rain;
	dr_t	xe_snow;

	bool	modern_driver_avail;
	dr_t	modern_driver;
} drs;

#define	RAIN_COMP_PHASE			xplm_Phase_Gauges
#define	RAIN_COMP_BEFORE		1

#define	RAIN_PAINT_PHASE		xplm_Phase_Gauges
#define	RAIN_PAINT_BEFORE		0

#define	MTX_CAPTURE_PHASE_LEGACY	xplm_Phase_Airplanes
#define	MTX_CAPTURE_BEFORE_LEGACY	false

#define	MTX_CAPTURE_PHASE_MODERN	xplm_Phase_Modern3D
#define	MTX_CAPTURE_BEFORE_MODERN	false

static void init_glass_stencil(glass_info_t *gi, GLuint fbo,
    unsigned w, unsigned h);
static void setup_smudge_tex(void);
static void destroy_smudge_tex(void);

static bool
using_modern_driver(void)
{
	return (drs.modern_driver_avail && dr_geti(&drs.modern_driver) != 0);
}

static bool
using_rev_float_z(void)
{
	return (drs.rev_float_z_avail && dr_geti(&drs.rev_float_z) != 0);
}

static bool
using_rev_y(void)
{
	return (drs.rev_y_avail && dr_geti(&drs.rev_y) != 0);
}

static void
bind_droplets_vtx_attrs(void)
{
	const droplets_paint_loc_t *loc = &droplets_paint_prog_loc;

	glutils_enable_vtx_attr_ptr(loc->pos, 2, GL_FLOAT, GL_FALSE,
	    sizeof (droplet_vtx_t), offsetof(droplet_vtx_t, pos));
	glutils_enable_vtx_attr_ptr(loc->ctr, 2, GL_FLOAT, GL_FALSE,
	    sizeof (droplet_vtx_t), offsetof(droplet_vtx_t, ctr));
	glutils_enable_vtx_attr_ptr(loc->radius, 1, GL_FLOAT, GL_FALSE,
	    sizeof (droplet_vtx_t), offsetof(droplet_vtx_t, radius));
	glutils_enable_vtx_attr_ptr(loc->size, 1, GL_FLOAT, GL_FALSE,
	    sizeof (droplet_vtx_t), offsetof(droplet_vtx_t, size));
}

static void
unbind_droplets_vtx_attrs(void)
{
	glutils_disable_vtx_attr_ptr(droplets_paint_prog_loc.pos);
	glutils_disable_vtx_attr_ptr(droplets_paint_prog_loc.ctr);
	glutils_disable_vtx_attr_ptr(droplets_paint_prog_loc.radius);
	glutils_disable_vtx_attr_ptr(droplets_paint_prog_loc.size);
}

static void
bind_tails_vtx_attrs(void)
{
	const tails_paint_loc_t *loc = &tails_prog_loc;

	glutils_enable_vtx_attr_ptr(loc->pos, 2, GL_FLOAT, GL_FALSE,
	    sizeof (droplet_tail_t), offsetof(droplet_tail_t, pos));
	glutils_enable_vtx_attr_ptr(loc->quant, 1, GL_FLOAT, GL_FALSE,
	    sizeof (droplet_tail_t), offsetof(droplet_tail_t, quant));
}

static void
unbind_tails_vtx_attrs(void)
{
	glutils_disable_vtx_attr_ptr(tails_prog_loc.pos);
	glutils_disable_vtx_attr_ptr(tails_prog_loc.quant);
}

static bool_t
have_compute(void)
{
	return (GLEW_ARB_compute_shader &&
	    GLEW_ARB_shader_storage_buffer_object &&
	    GLEW_ARB_shader_image_load_store);
}

static void
check_librain_init(void)
{
	ASSERT_MSG(inited, "librain not initialized or init failed. "
	    "Please call librain_init and check its return value to see "
	    "the init failed.%s", "");
}

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

	gi->gp = vect2_scmul(gi->glass->gravity_point, DEPTH_TEX_SZ(gi));
	gi->tp = vect2_scmul(gi->glass->thrust_point, DEPTH_TEX_SZ(gi));
	gi->wp = vect2_scmul(gi->glass->wind_point, DEPTH_TEX_SZ(gi));

	wind_comp = vect2_rot(wind_comp,
	    normalize_hdg(dr_getf(&drs.wind_dir)) - dr_getf(&drs.hdg));
	wind_comp = vect2_scmul(wind_comp, gi->glass->wind_factor);

	gs_comp = vect2_rot(gs_comp, dr_getf(&drs.beta));
	total_wind = vect2_add(wind_comp, gs_comp);

	total_wind = vect2_scmul(total_wind, 1.0 / gi->glass->max_tas);

	if (gi->glass->thrust_factor > 0 && gi->glass->max_thrust > 0) {
		thrust_v = vect2_scmul(VECT2(0,
		    MAX(dr_getf(&drs.prop_thrust) / gi->glass->max_thrust, 0)),
		    gi->glass->thrust_factor);
	} else {
		thrust_v = ZERO_VECT2;
	}

	gi->wind = clamp(vect2_abs(total_wind), 0, 1);

	gi->thrust = clamp(vect2_abs(thrust_v), 0, 1);
	if (vect2_abs(total_wind) > 0.001)
		gi->wp = vect2_rot(gi->wp, dir2hdg(total_wind));

	gi->gp = vect2_rot(gi->gp, clamp(-2 * rot_rate, -30, 30));

	gi->compute.wp = vect2_scmul(gi->wp, 1.0 / DEPTH_TEX_SZ(gi));
}

static void
update_glob_data(const mat4 proj_matrix, const mat4 acf_matrix, const vec4 vp)
{
	memcpy(glob_proj_matrix, proj_matrix, sizeof (mat4));
	if (!drs.VR_enabled_avail || dr_geti(&drs.VR_enabled) == 0) {
		/*
		 * Outside of VR, the projection parameters are placing
		 * the near clipping plane very far (about 1 meter), so
		 * we bring it much closer to get it fixed up.
		 */
		for (int i = 0; i < 4; i++)
			glob_proj_matrix[3][i] /= 100;
	}
	memcpy(glob_acf_matrix, acf_matrix, sizeof (mat4));
	glm_mat4_mul(glob_proj_matrix, glob_acf_matrix, glob_pvm);
	memcpy(glob_vp, vp, sizeof (vec4));
	for (int i = 0; i < 4; i++)
		cur_vp[i] = vp[i];
}

static void
update_ss_tex(void)
{
	if (cur_vp[2] == ss_texsz[0] && cur_vp[3] == ss_texsz[1])
		return;

	IF_TEXSZ(TEXSZ_FREE(librain_screenshot_tex, GL_RGB, GL_UNSIGNED_BYTE,
	    ss_texsz[0], ss_texsz[1]));
	destroy_smudge_tex();

	/* If the viewport size has changed, update the textures. */
	ss_texsz[0] = cur_vp[2];
	ss_texsz[1] = cur_vp[3];

	glBindTexture(GL_TEXTURE_2D, screenshot_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, ss_texsz[0], ss_texsz[1],
	    0, GL_RGB, GL_UNSIGNED_BYTE, NULL);

	IF_TEXSZ(TEXSZ_ALLOC(librain_screenshot_tex, GL_RGB, GL_UNSIGNED_BYTE,
	    ss_texsz[0], ss_texsz[1]));

	setup_smudge_tex();

	GLUTILS_ASSERT_NO_ERROR();
}

GLint
librain_get_current_fbo(void)
{
	GLint fbo;

	if (drs.current_gl_fbo_avail)
		fbo = dr_geti(&drs.current_gl_fbo);
	else
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);

	return (fbo);
}

void
librain_get_current_vp(GLint vp[4])
{
	int xp_vp[4];

	ASSERT(vp != NULL);
	VERIFY3S(dr_getvi(&drs.viewport, xp_vp, 0, 4), ==, 4);

	vp[0] = xp_vp[0];		/* left */
	vp[1] = xp_vp[1];		/* bottom */
	vp[2] = xp_vp[2] - xp_vp[0];	/* width */
	vp[3] = xp_vp[3] - xp_vp[1];	/* height */
}

void
librain_refresh_screenshot(void)
{
	GLint old_fbo = librain_get_current_fbo();

	glutils_debug_push(0, "librain_refresh_screenshot");

	glBindFramebufferEXT(GL_READ_FRAMEBUFFER, old_fbo);
	glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER, screenshot_fbo);
	glBlitFramebuffer(cur_vp[0], cur_vp[1], cur_vp[0] + cur_vp[2],
	    cur_vp[1] + cur_vp[3], 0, 0, ss_texsz[0], ss_texsz[1],
	    GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER, old_fbo);

	glutils_debug_pop();

	GLUTILS_ASSERT_NO_ERROR();
}

void
librain_draw_z_depth(obj8_t *obj, const char **z_depth_group_ids)
{
	check_librain_init();

	if (!prepare_ran)
		return;

	glutils_debug_push(0, "librain_draw_z_depth(%s)",
	    lacf_basename(obj8_get_filename(obj)));

	if (!debug_draw)
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glUseProgram(z_depth_prog);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	if (z_depth_group_ids != NULL) {
		for (int i = 0; z_depth_group_ids[i] != NULL; i++) {
			glutils_debug_push(0, "librain_draw_z_depth(%s, %s)",
			    lacf_basename(obj8_get_filename(obj)),
			    z_depth_group_ids[i]);
			obj8_draw_group(obj, z_depth_group_ids[i],
			    z_depth_prog, glob_pvm);
			glutils_debug_pop();
		}
	} else {
		glutils_debug_push(0, "librain_draw_z_depth(%s, NULL)",
		    lacf_basename(obj8_get_filename(obj)));
		obj8_draw_group(obj, NULL, z_depth_prog, glob_pvm);
		glutils_debug_pop();
	}
	glUseProgram(0);
	if (!debug_draw)
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	glutils_debug_pop();
}

static void
ws_temp_comp(glass_info_t *gi)
{
	float now = dr_getf(&drs.sim_time);
	float d_t = now - gi->last_ws_temp_t;
	float rand_seed;
	ws_temp_prog_loc_t *loc = &ws_temp_prog_loc;

	if (d_t < WS_TEMP_COMP_INTVAL)
		return;

	glutils_debug_push(0, "ws_temp_comp(%s)", GLASS_NAME(gi));

	rand_seed = crc64_rand_fract();

	XPLMSetGraphicsState(0, 1, 0, 1, 1, 1, 1);

	glBindFramebufferEXT(GL_FRAMEBUFFER, gi->ws_temp_fbo[!gi->ws_temp_cur]);
	glViewport(0, 0, WS_TEMP_TEX_W, WS_TEMP_TEX_H);

	glUseProgram(ws_temp_prog);

	glUniformMatrix4fv(loc->pvm, 1, GL_FALSE, (void *)gi->ws_temp_pvm);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, gi->ws_temp_tex[gi->ws_temp_cur]);
	glUniform1i(loc->src, 0);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, gi->water_depth_tex[gi->water_depth_cur]);
	glUniform1i(loc->depth, 1);

	glUniform1f(loc->rand_seed, rand_seed);
	glUniform1f(loc->le_temp, C2KELVIN(dr_getf(&drs.le_temp)));
	glUniform1f(loc->cabin_temp, gi->glass->cabin_temp);
	glUniform1f(loc->wind_fact, MAX(gi->wind, gi->thrust));
	glUniform1f(loc->d_t, d_t);
	glUniform1f(loc->inertia_in, gi->glass->therm_inertia);
	glUniform4fv(loc->heat_zones, 16, gi->glass->heat_zones);
	glUniform1fv(loc->heat_tgt_temps, 4, gi->glass->heat_tgt_temps);
	glUniform1f(loc->precip_intens,
	    precip_intens * gi->glass->slant_factor);
	glUniform2fv(loc->hot_air_src, 4, gi->glass->hot_air_src);
	glUniform1fv(loc->hot_air_radius, 2, gi->glass->hot_air_radius);
	glUniform1fv(loc->hot_air_temp, 2, gi->glass->hot_air_temp);

	glutils_draw_quads(&gi->ws_temp_quads, ws_temp_prog);

	glUseProgram(0);
	glActiveTexture(GL_TEXTURE0);

	gi->ws_temp_cur = !gi->ws_temp_cur;
	gi->last_ws_temp_t = now;

	glutils_debug_pop();
}

static void
wiper_setup_prog_one(const glass_info_t *gi, GLuint prog, unsigned wiper_i,
    unsigned wiper_prog_i, double pos_cur, double pos_prev)
{
	char name[32];
	const librain_glass_t *glass = gi->glass;

	snprintf(name, sizeof (name), "wiper_pivot[%d]", wiper_prog_i);
	glUniform2f(glGetUniformLocation(prog, name),
	    glass->wiper_pivot[wiper_i].x, glass->wiper_pivot[wiper_i].y);

	snprintf(name, sizeof (name), "wiper_radius_outer[%d]", wiper_prog_i);
	glUniform1f(glGetUniformLocation(prog, name),
	    glass->wiper_radius_outer[wiper_i]);

	snprintf(name, sizeof (name), "wiper_radius_inner[%d]", wiper_prog_i);
	glUniform1f(glGetUniformLocation(prog, name),
	    glass->wiper_radius_inner[wiper_i]);

	snprintf(name, sizeof (name), "wiper_pos_cur[%d]", wiper_prog_i);
	glUniform1f(glGetUniformLocation(prog, name), pos_cur);

	snprintf(name, sizeof (name), "wiper_pos_prev[%d]", wiper_prog_i);
	glUniform1f(glGetUniformLocation(prog, name), pos_prev);

	snprintf(name, sizeof (name), "wiper_pos_prev[%d]", wiper_prog_i);
	glUniform1f(glGetUniformLocation(prog, name), pos_prev);
}

static void
wiper_setup_prog(const glass_info_t *gi, GLuint prog)
{
	unsigned num_wipers = 0;

	for (int i = 0; i < MAX_WIPERS; i++) {
		const wiper_t *wiper = &gi->wipers[i];

		/* Skip the wiper if it isn't in motion, or if is zero-size */
		if (wiper->angle_prev == wiper->angle_now ||
		    gi->glass->wiper_radius_outer[0] ==
		    gi->glass->wiper_radius_inner[0])
			continue;

		if (!isnan(wiper->angle_edge)) {
			/* Reversing at end of travel, emit two zones */
			wiper_setup_prog_one(gi, prog, i, num_wipers,
			    wiper->angle_now, wiper->angle_edge);
			num_wipers++;
			wiper_setup_prog_one(gi, prog, i, num_wipers,
			    wiper->angle_edge, wiper->angle_smear);
			num_wipers++;
		} else {
			/* In the middle of traverse, emit one zone */
			wiper_setup_prog_one(gi, prog, i, num_wipers,
			    wiper->angle_now, wiper->angle_smear);
			num_wipers++;
		}
	}

	glUniform1i(glGetUniformLocation(prog, "num_wipers"), num_wipers);
}

static void
rain_stage1_comp_legacy(glass_info_t *gi, double d_t, float rand_seed)
{
	GLuint prog = rain_stage1_prog;
	const rain_stage1_loc_t *loc = &rain_stage1_loc;

	glutils_debug_push(0, "rain_stage1_comp_legacy(%s)", GLASS_NAME(gi));

	glUseProgram(prog);

	glUniformMatrix4fv(loc->pvm, 1, GL_FALSE, (void *)gi->water_depth_pvm);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, gi->water_depth_tex[gi->water_depth_cur]);
	glUniform1i(loc->tex, 0);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, gi->ws_temp_tex[gi->ws_temp_cur]);
	glUniform1i(loc->temp_tex, 1);

	glUniform1f(loc->rand_seed, rand_seed);
	glUniform1f(loc->precip_intens,
	    precip_intens * gi->glass->slant_factor);
	glUniform1f(loc->thrust, gi->thrust);
	glUniform1f(loc->wind, gi->wind);
	glUniform1f(loc->gravity, (float)gi->glass->gravity_factor);
	glUniform1f(loc->d_t, d_t);
	glUniform2f(loc->gp, gi->gp.x, gi->gp.y);
	glUniform2f(loc->tp, gi->tp.x, gi->tp.y);
	glUniform2f(loc->wp, gi->wp.x, gi->wp.y);
	glUniform1f(loc->wind_temp, C2KELVIN(dr_getf(&drs.amb_temp)));

	wiper_setup_prog(gi, prog);

	glutils_draw_quads(&gi->water_depth_quads, prog);

	glUseProgram(0);

	glutils_debug_pop();
}

static void
rain_stage1_compute_move(const glass_info_t *gi, double cur_t, double d_t)
{
#define	GRAVITY_SCALE_FACTOR	0.15
	GLuint prog = droplets_prog;
	GLuint ws_temp_tex = gi->ws_temp_tex[gi->ws_temp_cur];
	double gravity_force, wind_force, thrust_force;
	float min_droplet_sz;
	float rand_seed[NUM_RANDOM_SEEDS];
	const droplets_move_loc_t *loc = &droplets_move_loc;

	glutils_debug_push(0, "rain_stage1_compute_move(%s)", GLASS_NAME(gi));

	for (int i = 0; i < NUM_RANDOM_SEEDS; i++)
		rand_seed[i] = (crc64_rand() % 1000000) / 1000000.0;

	gravity_force = GRAVITY_SCALE_FACTOR * (1.5 - gi->glass->slant_factor);

	wind_force = gi->wind;
	thrust_force = gi->thrust;

	min_droplet_sz = wavg(MIN_DROPLET_SZ_MIN, MIN_DROPLET_SZ_MAX,
	    precip_intens);

	glUseProgram(prog);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, ws_temp_tex);
	glUniform1i(loc->ws_temp_tex, 0);

	glUniform1f(loc->cur_t, cur_t);
	glUniform1f(loc->d_t, d_t);
	glUniform1fv(loc->rand_seed, NUM_RANDOM_SEEDS, rand_seed);
	glUniform2f(loc->gravity_point,
	    gi->glass->gravity_point.x, gi->glass->gravity_point.y);
	glUniform1f(loc->gravity_force, gravity_force);
	glUniform2f(loc->wind_point, gi->compute.wp.x, gi->compute.wp.y);
	glUniform1f(loc->wind_force, wind_force);
	glUniform2f(loc->thrust_point,
	    gi->glass->thrust_point.x, gi->glass->thrust_point.y);
	glUniform1f(loc->thrust_force, thrust_force);
	glUniform1f(loc->precip_intens, pow(precip_intens, 0.7));
	glUniform1f(loc->min_droplet_sz, min_droplet_sz);
	glUniform1f(loc->le_temp, C2KELVIN(dr_getf(&drs.le_temp)));

	glBindBufferBase(GL_SHADER_STORAGE_BUFFER,
	    DROPLETS_SSBO_BINDING, gi->droplets_ssbo);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER,
	    VERTICES_SSBO_BINDING, gi->vertices_vtx_buf);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER,
	    TAILS_SSBO_BINDING, gi->tails_vtx_buf);

	glDispatchCompute(gi->qual.num_droplets / DROPLET_WG_SIZE, 1, 1);

	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, DROPLETS_SSBO_BINDING, 0);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, VERTICES_SSBO_BINDING, 0);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TAILS_SSBO_BINDING, 0);

	glutils_debug_pop();
}

static void
rain_stage1_comp(glass_info_t *gi)
{
	enum { MIN_FPS = 5 };
	float cur_t = dr_getf(&drs.sim_time);
	float d_t = cur_t - gi->last_stage1_t;

	glutils_debug_push(0, "rain_stage1_comp(%s)", GLASS_NAME(gi));

	if (gi->last_stage1_t == 0.0)
		gi->last_stage1_t = cur_t;
	/* Prevent running at too low FPS, because the effect can be weird */
	d_t = MIN(cur_t - gi->last_stage1_t, 1.0 / MIN_FPS);

	/*
	 * When time isn't progressing (sim is paused or the user is
	 * in some setup screen), avoid running, because the water
	 * droplets can't move anyway.
	 */
	if (d_t > 0.0) {
		float rand_seed = (crc64_rand() % 1000000) / 1000000.0;

		glBindFramebufferEXT(GL_FRAMEBUFFER,
		    gi->water_depth_fbo[!gi->water_depth_cur]);
		glViewport(0, 0, DEPTH_TEX_SZ(gi), DEPTH_TEX_SZ(gi));
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		if (!gi->qual.use_compute)
			rain_stage1_comp_legacy(gi, d_t, rand_seed);
		else
			rain_stage1_compute_move(gi, cur_t, d_t);

		glActiveTexture(GL_TEXTURE0);

		gi->stage1_adv = B_TRUE;
	} else {
		gi->stage1_adv = B_FALSE;
	}

	gi->last_stage1_t = cur_t;

	glutils_debug_pop();
}

static void
rain_stage2_compute_paint_droplets(const glass_info_t *gi)
{
	size_t num_elem = gi->qual.num_droplets * FACES_PER_DROPLET * 3;

	glutils_debug_push(0, "rain_stage2_compute_paint(%s, droplets)",
	    GLASS_NAME(gi));

	glUseProgram(droplets_paint_prog);

	glBindBuffer(GL_ARRAY_BUFFER, gi->vertices_vtx_buf);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gi->vertices_idx_buf);

	bind_droplets_vtx_attrs();
	glDrawElements(GL_TRIANGLES, num_elem, GL_UNSIGNED_INT, NULL);
	unbind_droplets_vtx_attrs();

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	glutils_debug_pop();
}

static void
rain_stage2_compute_paint_tails(const glass_info_t *gi)
{
	size_t num_elem = NUM_STREAMERS(gi->qual.num_droplets) *
	    NUM_DROPLET_TAIL_SEGS * 2;

	glutils_debug_push(0, "rain_stage2_compute_paint(%s, tails)",
	    GLASS_NAME(gi));

	glUseProgram(tails_prog);
	glBindBuffer(GL_ARRAY_BUFFER, gi->tails_vtx_buf);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gi->tails_idx_buf);
	glLineWidth(STREAMER_WIDTH);

	bind_tails_vtx_attrs();
	glDrawElements(GL_LINES, num_elem, GL_UNSIGNED_INT, NULL);
	unbind_tails_vtx_attrs();

	glLineWidth(1);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	glutils_debug_pop();
}

static void
rain_stage2_compute_paint(glass_info_t *gi)
{
	glBindFramebufferEXT(GL_FRAMEBUFFER,
	    gi->water_depth_fbo[!gi->water_depth_cur]);
	glViewport(0, 0, DEPTH_TEX_SZ(gi), DEPTH_TEX_SZ(gi));

	glEnable(GL_STENCIL_TEST);
	glStencilFunc(GL_EQUAL, 1, 0xFF);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);

	rain_stage2_compute_paint_droplets(gi);
	rain_stage2_compute_paint_tails(gi);

	glDisable(GL_STENCIL_TEST);
}

static void
rain_stage2_normals(glass_info_t *gi)
{
	const stage2_loc_t *loc =
	    (gi->qual.use_compute ? &stage2_comp_loc : &stage2_loc);
	GLuint prog =
	    (gi->qual.use_compute ? rain_stage2_comp_prog : rain_stage2_prog);

	glutils_debug_push(0, "rain_stage2_normals");

	glBindFramebufferEXT(GL_FRAMEBUFFER, gi->water_norm_fbo);
	glViewport(0, 0, NORM_TEX_SZ(gi), NORM_TEX_SZ(gi));
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glUseProgram(prog);

	glUniformMatrix4fv(loc->pvm, 1, GL_FALSE, (void *)gi->water_norm_pvm);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, gi->water_depth_tex[!gi->water_depth_cur]);
	glUniform1i(loc->tex, 0);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, gi->ws_temp_tex[gi->ws_temp_cur]);
	glUniform1i(loc->temp_tex, 1);

	glUniform2f(loc->tp, gi->tp.x, gi->tp.y);
	glUniform1f(loc->thrust, gi->thrust);
	glUniform2f(loc->wp, gi->wp.x, gi->wp.y);
	glUniform1f(loc->wind, gi->wind);
	glUniform1f(loc->precip_intens,
	    precip_intens * gi->glass->slant_factor);
	glUniform1f(loc->window_ice, dr_getf(&drs.window_ice));

	glutils_draw_quads(&gi->water_norm_quads, prog);

	glUseProgram(0);
	glActiveTexture(GL_TEXTURE0);

	glBindTexture(GL_TEXTURE_2D, gi->water_norm_tex);
	glGenerateMipmap(GL_TEXTURE_2D);

	glBindTexture(GL_TEXTURE_2D, 0);
	glutils_debug_pop();
}

static void
rain_stage2_comp(glass_info_t *gi)
{
	if (gi->stage1_adv) {
		glutils_debug_push(0, "rain_stage2_comp");

		if (gi->qual.use_compute)
			rain_stage2_compute_paint(gi);
		rain_stage2_normals(gi);

		glutils_debug_pop();
	}
}

static bool_t
rain_should_draw(void)
{
	return (dr_getf(&drs.sim_time) - last_rain_t < RAIN_DRAW_TIMEOUT &&
	    rain_enabled);
}

static void
gl_state_reset(void)
{
	GLUTILS_RESET_ERRORS();
	glutils_disable_all_vtx_attrs();
}

static int
rain_comp_cb(XPLMDrawingPhase phase, int before, void *refcon)
{
	GLint vp[4];
	GLint old_fbo;

	UNUSED(phase);
	UNUSED(before);
	UNUSED(refcon);

	/* Make sure we only run the computations once per frame */
	if (dr_geti(&drs.panel_render_type) != PANEL_RENDER_TYPE_3D_UNLIT)
		return (1);

	if (!rain_should_draw()) {
		for (size_t i = 0; i < num_glass_infos; i++) {
			glass_info_t *gi = &glass_infos[i];
			gi->last_stage1_t = dr_getf(&drs.sim_time);
		}
		return (1);
	}

	gl_state_reset();
	librain_get_current_vp(vp);
	old_fbo = librain_get_current_fbo();

	for (size_t i = 0; i < num_glass_infos; i++) {
		glass_info_t *gi = &glass_infos[i];
		if (gi->qual.use_compute) {
			/*
			 * If any single glass surface uses a compute shader,
			 * emit a barrier so that any previous shader
			 * invocations have certainly completed by now.
			 */
			glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
			break;
		}
	}
	for (size_t i = 0; i < num_glass_infos; i++) {
		glass_info_t *gi = &glass_infos[i];

		update_vectors(gi);
		ws_temp_comp(gi);
		rain_stage1_comp(gi);
	}

	gl_state_cleanup();
	glBindFramebufferEXT(GL_FRAMEBUFFER, old_fbo);
	glViewport(vp[0], vp[1], vp[2], vp[3]);

	GLUTILS_ASSERT_NO_ERROR();

	return (1);
}

static int
rain_paint_cb(XPLMDrawingPhase phase, int before, void *refcon)
{
	GLint vp[4];
	GLint old_fbo;

	UNUSED(phase);
	UNUSED(before);
	UNUSED(refcon);

	/* Make sure we only run the computations once per frame */
	if (dr_geti(&drs.panel_render_type) != PANEL_RENDER_TYPE_3D_UNLIT ||
	    !rain_should_draw()) {
		return (1);
	}

	gl_state_reset();
	librain_get_current_vp(vp);
	old_fbo = librain_get_current_fbo();

	for (size_t i = 0; i < num_glass_infos; i++)
		rain_stage2_comp(&glass_infos[i]);

	gl_state_cleanup();
	glBindFramebufferEXT(GL_FRAMEBUFFER, old_fbo);
	glViewport(vp[0], vp[1], vp[2], vp[3]);

	GLUTILS_ASSERT_NO_ERROR();

	return (1);
}

static void
update_mtx_info(unsigned idx)
{
	int vp[4];

	ASSERT3U(idx, <, 2);

	VERIFY3S(dr_getvf32(&drs.acf_matrix,
	    (float *)mtx_info[idx].acf_matrix, 0, 16), ==, 16);
	VERIFY3S(dr_getvf32(&drs.proj_matrix,
	    (float *)mtx_info[idx].proj_matrix, 0, 16), ==, 16);
	VERIFY3S(dr_getvi(&drs.viewport, vp, 0, 4), ==, 4);
	if (drs.aa_ratio_avail) {
		/*
		 * Anti-aliasing messes with the viewport, so we need to
		 * divide it by the AA ratio, otherwise our render doesn't
		 * line up with the screen.
		 */
		float ratio_x = dr_getf(&drs.fsaa_ratio_x);
		float ratio_y = dr_getf(&drs.fsaa_ratio_y);
		vp[0] /= ratio_x;
		vp[1] /= ratio_y;
		vp[2] /= ratio_x;
		vp[3] /= ratio_y;
	}
	for (int i = 0; i < 4; i++)
		mtx_info[idx].viewport[i] = vp[i];
}

int
capture_mtx(XPLMDrawingPhase phase, int before, void *refcon)
{
	int idx;
	draw_call_type_t dct;

	UNUSED(phase);
	UNUSED(before);
	UNUSED(refcon);
	/*
	 * No GL calls take place here, so no need for GLUTILS_RESET_ERRORS()
	 */
	cached_rev_float_z = using_rev_float_z();
	cached_rev_y = using_rev_y();

	if (!using_modern_driver() &&
	    (dr_geti(&drs.plane_render_type) != PLANE_RENDER_SOLID ||
	    dr_geti(&drs.world_render_type) == WORLD_RENDER_TYPE_REFLECT)) {
		return (1);
	}
	dct = dr_geti(&drs.draw_call_type);
	if (dct == DRAW_CALL_RIGHT_EYE) {
		idx = 1;
		num_mtx_info = 2;
	} else if (dct == DRAW_CALL_LEFT_EYE) {
		idx = 0;
		num_mtx_info = 2;
	} else {
		idx = 0;
		num_mtx_info = 1;
	}
	update_mtx_info(idx);

	return (1);
}

static bool_t
wiper_debug_add(GLuint prog, const glass_info_t *gi, int i)
{
	char name[32];

	if (gi->glass->wiper_radius_outer[i] ==
	    gi->glass->wiper_radius_inner[i])
		return (B_FALSE);

	snprintf(name, sizeof (name), "wiper_pivot[%d]", i);
	glUniform2f(glGetUniformLocation(prog, name),
	    gi->glass->wiper_pivot[i].x, gi->glass->wiper_pivot[i].y);

	snprintf(name, sizeof (name), "wiper_radius_outer[%d]", i);
	glUniform1f(glGetUniformLocation(prog, name),
	    gi->glass->wiper_radius_outer[i]);

	snprintf(name, sizeof (name), "wiper_radius_inner[%d]", i);
	glUniform1f(glGetUniformLocation(prog, name),
	    gi->glass->wiper_radius_inner[i]);

	snprintf(name, sizeof (name), "wiper_pos[%d]", i);
	glUniform1f(glGetUniformLocation(prog, name), gi->wipers[i].angle_now);

	return (B_TRUE);
}

static void
draw_ws_effects(glass_info_t *gi, GLint old_fbo)
{
	GLuint prog = (gi->qual.use_compute ? ws_rain_comp_prog : ws_rain_prog);

	glutils_debug_push(0, "draw_ws_effects(%s)", GLASS_NAME(gi));

	glEnable(GL_DEPTH_TEST);

	/*
	 * Pre-stage: render the actual image to a side buffer, but without
	 * any smudging.
	 */
	glBindFramebufferEXT(GL_FRAMEBUFFER, ws_smudge_fbo);
	glViewport(0, 0, ss_texsz[0], ss_texsz[1]);

	glUseProgram(prog);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, gi->water_norm_tex);
	glUniform1i(glGetUniformLocation(prog, "norm_tex"), 0);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, screenshot_tex);
	glUniform1i(glGetUniformLocation(prog, "screenshot_tex"), 1);

	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, gi->water_depth_tex[!gi->water_depth_cur]);
	glUniform1i(glGetUniformLocation(prog, "depth_tex"), 2);

	glUniform4f(glGetUniformLocation(prog, "vp"),
	    cur_vp[0], cur_vp[1], cur_vp[2], cur_vp[3]);

	if (wipers_visible) {
		unsigned num_wipers = 0;
		for (int i = 0; i < MAX_WIPERS; i++) {
			if (wiper_debug_add(prog, gi, i))
				num_wipers++;
		}
		glUniform1i(glGetUniformLocation(prog, "num_wipers"),
		    num_wipers);
	} else {
		glUniform1i(glGetUniformLocation(prog, "num_wipers"), 0);
	}

	if (gi->glass->group_ids != NULL) {
		for (int i = 0; gi->glass->group_ids[i] != NULL; i++) {
			glutils_debug_push(0, "ws_rain(%s)",
			    gi->glass->group_ids[i]);
			obj8_draw_group(gi->glass->obj, gi->glass->group_ids[i],
			    prog, glob_pvm);
			glutils_debug_pop();
		}
	} else {
		glutils_debug_push(0, "ws_rain(NULL)");
		obj8_draw_group(gi->glass->obj, NULL, prog, glob_pvm);
		glutils_debug_pop();
	}

	/* Restore old framebuffer */
	glBindFramebufferEXT(GL_FRAMEBUFFER, old_fbo);
	glViewport(cur_vp[0], cur_vp[1], cur_vp[2], cur_vp[3]);

#if	APL
	/*
	 * Mac depth buffer problem workaround.
	 */
	if (!rain_should_draw())
		goto out;
#endif

	/*
	 * Final stage: render the prepped displaced texture and apply
	 * variable smudging based on water depth.
	 */
	prog = (gi->qual.use_compute ? ws_smudge_comp_prog : ws_smudge_prog);
	glUseProgram(prog);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, screenshot_tex);
	glUniform1i(glGetUniformLocation(prog, "screenshot_tex"), 0);
	glUniform2f(glGetUniformLocation(prog, "screenshot_tex_sz"),
	    ss_texsz[0], ss_texsz[1]);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, ws_smudge_tex);
	glUniform1i(glGetUniformLocation(prog, "ws_tex"), 1);

	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, gi->water_depth_tex[!gi->water_depth_cur]);
	glUniform1i(glGetUniformLocation(prog, "depth_tex"), 2);

	glUniform4f(glGetUniformLocation(prog, "vp"),
	    cur_vp[0], cur_vp[1], cur_vp[2], cur_vp[3]);

	if (gi->glass->group_ids != NULL) {
		for (int i = 0; gi->glass->group_ids[i] != NULL; i++) {
			glutils_debug_push(0, "ws_smudge(%s)",
			    gi->glass->group_ids[i]);
			obj8_draw_group(gi->glass->obj,
			    gi->glass->group_ids[i], prog, glob_pvm);
			glutils_debug_pop();
		}
	} else {
		glutils_debug_push(0, "ws_smudge(NULL)");
		obj8_draw_group(gi->glass->obj, NULL, prog, glob_pvm);
		glutils_debug_pop();
	}

#if	APL
	/*
	 * Mac depth buffer problem workaround.
	 */
out:
#endif

	glUseProgram(0);
	glActiveTexture(GL_TEXTURE0);

	glutils_debug_pop();
}

static void
compute_precip(double now)
{
	double d_t = now - last_run_t;

	if (d_t <= 0)
		return;

	if (drs.xe_present && dr_geti(&drs.xe_active) != 0) {
		precip_intens = MAX(dr_getf(&drs.xe_rain),
		    dr_getf(&drs.xe_snow));
	} else {
		precip_intens = dr_getf(&drs.precip_rat);
	}

	if (precip_intens > 0.0)
		last_rain_t = now;

	last_run_t = now;
}

unsigned
librain_get_call_count(void)
{
	if (xp_ver < 11500)
		return (num_mtx_info);

	switch (dr_geti(&drs.draw_call_type)) {
	case DRAW_CALL_LEFT_EYE:
	case DRAW_CALL_RIGHT_EYE:
		return (2);
	default:
		return (1);
	}
}

static void
setup_priv_depth_buf(unsigned w, unsigned h)
{
	if (priv_depth_tex == 0) {
		glGenTextures(1, &priv_depth_tex);
		ASSERT(priv_depth_tex != 0);
	}
	if (w != priv_depth_tex_w || h != priv_depth_tex_h) {
		glBindTexture(GL_TEXTURE_2D, priv_depth_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, w, h, 0,
		    GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
		priv_depth_tex_w = w;
		priv_depth_tex_h = h;
	}
}

void
librain_reset_clip_control(void)
{
	if (cached_rev_float_z) {
		if (cached_rev_y)
			glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
		else
			glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
	} else {
		glClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
	}
}

void
librain_draw_prepare_all(void)
{
	double now;
	GLint depth_type;

	check_librain_init();

	now = dr_getf(&drs.sim_time);

	compute_precip(now);

	librain_get_current_vp(saved_vp);

	if (cached_rev_float_z) {
		glGetIntegerv(GL_CLIP_ORIGIN, (GLint *)&saved_clip_origin);
		glGetIntegerv(GL_CLIP_DEPTH_MODE, &saved_depth_mode);
		glGetIntegerv(GL_DEPTH_FUNC, &saved_depth_func);
		glGetFloatv(GL_DEPTH_CLEAR_VALUE, &saved_depth_clear);

		glDepthFunc(GL_GEQUAL);
		glClearDepth(0.0);

		if (cached_rev_y) {
			glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);

			glGetIntegerv(GL_FRONT_FACE, &saved_front_face);
			glFrontFace(GL_CCW);
		} else {
			glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
		}
	}
	if (using_modern_driver()) {
		glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
		    GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
		    &depth_type);
		if (depth_type == GL_NONE) {
			/*
			 * X-Plane's FBO is missing a depth buffer. Probably
			 * because we were called in a 2D phase. We need to
			 * create our own.
			 */
			setup_priv_depth_buf(saved_vp[2], saved_vp[3]);
			ASSERT(priv_depth_tex != 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
			    GL_TEXTURE_2D, priv_depth_tex, 0);
			priv_depth_override = true;
		}
	}

	GLUTILS_ASSERT_NO_ERROR();
}

void
librain_draw_prepare_eye(unsigned call_index, bool_t force)
{
	double now = dr_getf(&drs.sim_time);

	check_librain_init();

	ASSERT3U(call_index, <, num_mtx_info);
	update_glob_data(mtx_info[call_index].proj_matrix,
	    mtx_info[call_index].acf_matrix,
	    mtx_info[call_index].viewport);

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

	glob_call_index = call_index;

	glViewport(mtx_info[call_index].viewport[0],
	    mtx_info[call_index].viewport[1],
	    mtx_info[call_index].viewport[2],
	    mtx_info[call_index].viewport[3]);

	glClear(GL_DEPTH_BUFFER_BIT);

	update_ss_tex();
	librain_refresh_screenshot();

	GLUTILS_ASSERT_NO_ERROR();
}

void
librain_draw_exec(void)
{
	GLint old_fbo;

	check_librain_init();

#if	!APL
	/*
	 * The Mac fucks with the depth buffer unless we draw something here,
	 * so fuck it, we'll always draw.
	 */
	if (!rain_should_draw())
		return;
#endif	/* !APL */

	old_fbo = librain_get_current_fbo();

	for (size_t i = 0; i < num_glass_infos; i++)
		draw_ws_effects(&glass_infos[i], old_fbo);

	glBindFramebufferEXT(GL_FRAMEBUFFER, old_fbo);
	gl_state_cleanup();

	GLUTILS_ASSERT_NO_ERROR();
}

void
librain_draw_finish_all(void)
{
	check_librain_init();

	for (size_t i = 0; i < num_glass_infos; i++) {
		glass_info_t *gi = &glass_infos[i];
		if (gi->stage1_adv)
			gi->water_depth_cur = !gi->water_depth_cur;
	}
	glViewport(saved_vp[0], saved_vp[1], saved_vp[2], saved_vp[3]);

	if (cached_rev_float_z) {
		glClipControl(saved_clip_origin, saved_depth_mode);
		glDepthFunc(saved_depth_func);
		glClearDepth(saved_depth_clear);
		if (cached_rev_y)
			glFrontFace(saved_front_face);
	}
	if (priv_depth_override) {
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
		    GL_TEXTURE_2D, 0, 0);
		priv_depth_override = false;
	}

	GLUTILS_ASSERT_NO_ERROR();
}

void
librain_set_enabled(bool_t flag)
{
	rain_enabled = flag;
}

static void
reset_wiper(wiper_t *wiper)
{
	ASSERT(wiper != NULL);
	wiper->angle_prev = NAN;
	wiper->angle_edge = NAN;
	wiper->angle_smear = NAN;
}

static void
update_wiper(wiper_t *wiper, double angle, bool_t is_moving)
{
	double cur_t = dr_getf(&drs.sim_time);
	double d_t, ang_vel;
	bool_t dir_new;

	ASSERT(wiper != NULL);

	d_t = cur_t - wiper->angle_now_t;
	if (d_t <= 0)
		return;

	if (!is_moving) {
		reset_wiper(wiper);
		return;
	}

	if (isnan(wiper->angle_prev)) {
		wiper->angle_prev = angle;
		wiper->angle_now_t = cur_t;
		return;
	}
	wiper->angle_prev = wiper->angle_now;
	wiper->angle_now = angle;
	wiper->angle_now_t = cur_t;

	ang_vel = (wiper->angle_now - wiper->angle_prev) / d_t;
	dir_new = (ang_vel > 0);
	if (dir_new != wiper->dir) {
		wiper->angle_edge = wiper->angle_prev;
		wiper->angle_edge_t = cur_t;
		wiper->dir = dir_new;
	}
	if (cur_t - wiper->angle_edge_t >= WIPER_SMEAR_DELAY) {
		/* Stop tracking the edge. */
		wiper->angle_edge = NAN;
	}
	if (isnan(wiper->angle_smear)) {
		wiper->angle_smear = wiper->angle_prev;
		wiper->angle_smear_t = cur_t;
	}
	if (cur_t - wiper->angle_smear_t > WIPER_SMEAR_DELAY) {
		/*
		 * If the wiper has reversed, start moving towards the edge
		 * position, not the current wiper position.
		 */
		if (!isnan(wiper->angle_edge)) {
			FILTER_IN_LIN(wiper->angle_smear, wiper->angle_edge,
			    d_t, ABS(ang_vel));
		} else {
			FILTER_IN_LIN(wiper->angle_smear, wiper->angle_now,
			    d_t, ABS(ang_vel));
		}
		wiper->angle_smear_t = cur_t - WIPER_SMEAR_DELAY;
	}
}

LIBRAIN_EXPORT void
librain_set_wiper_angle(const librain_glass_t *glass, unsigned wiper_nr,
    double angle_radians, bool_t is_moving)
{
	ASSERT(glass != NULL);
	ASSERT3U(wiper_nr, <, MAX_WIPERS);
	ASSERT3F(angle_radians, >=, -M_PI);
	ASSERT3F(angle_radians, <=, M_PI);

	for (size_t i = 0; i < num_glass_infos; i++) {
		if (glass_infos[i].glass == glass) {
			update_wiper(&(glass_infos[i].wipers[wiper_nr]),
			    angle_radians, is_moving);
			return;
		}
	}
	VERIFY_MSG(0, "invalid librain_glass_t passed: %p (unknown pointer)",
	    glass);
}

static void
validate_glass(const librain_glass_t *glass)
{
	VERIFY(glass != NULL);
	VERIFY_MSG(glass->obj != NULL,
	    "You must NOT pass NULL for librain_glass_t->obj (%p)", glass);
	VERIFY(!isnan(glass->slant_factor));
	VERIFY(!IS_NULL_VECT(glass->thrust_point));
	VERIFY(!isnan(glass->thrust_factor));
	VERIFY3F(glass->max_thrust, >=, 0);
}

static void
glass_info_init_quads(glass_info_t *gi)
{
	vect2_t ws_temp_pos[] = {
	    VECT2(0, 0), VECT2(0, WS_TEMP_TEX_H),
	    VECT2(WS_TEMP_TEX_W, WS_TEMP_TEX_H),
	    VECT2(WS_TEMP_TEX_W, 0)
	};
	vect2_t water_depth_pos[] = {
	    VECT2(0, 0), VECT2(0, DEPTH_TEX_SZ(gi)),
	    VECT2(DEPTH_TEX_SZ(gi), DEPTH_TEX_SZ(gi)),
	    VECT2(DEPTH_TEX_SZ(gi), 0)
	};
	vect2_t water_norm_pos[] = {
	    VECT2(0, 0), VECT2(0, NORM_TEX_SZ(gi)),
	    VECT2(NORM_TEX_SZ(gi), NORM_TEX_SZ(gi)), VECT2(NORM_TEX_SZ(gi), 0)
	};

	glutils_init_2D_quads(&gi->ws_temp_quads, ws_temp_pos, NULL, 4);
	glutils_init_2D_quads(&gi->water_depth_quads, water_depth_pos, NULL, 4);
	glutils_init_2D_quads(&gi->water_norm_quads, water_norm_pos, NULL, 4);
}

static void
glass_info_init_compute_phys(glass_info_t *gi)
{
	double cur_t = dr_getf(&drs.sim_time);
	size_t droplet_bytes = gi->qual.num_droplets * sizeof (droplet_data_t);
	size_t vertex_bytes = gi->qual.num_droplets * VTX_PER_DROPLET *
	    sizeof (droplet_vtx_t);
	size_t tails_bytes = NUM_STREAMERS(gi->qual.num_droplets) *
	    NUM_DROPLET_HISTORY * sizeof (droplet_tail_t);
	droplet_data_t *droplets = safe_calloc(1, droplet_bytes);
	droplet_vtx_t *vertices = safe_calloc(1, vertex_bytes);
	droplet_tail_t *tails = safe_calloc(1, tails_bytes);

	for (unsigned i = 0; i < gi->qual.num_droplets; i++) {
		droplets[i].regen_t = cur_t;
		droplets[i].bump_t = cur_t + crc64_rand_fract();
	}

	glGenBuffers(1, &gi->droplets_ssbo);
	VERIFY(gi->droplets_ssbo);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, gi->droplets_ssbo);
	glBufferData(GL_SHADER_STORAGE_BUFFER, droplet_bytes, droplets,
	    GL_DYNAMIC_DRAW);

	glGenBuffers(1, &gi->vertices_vtx_buf);
	VERIFY(gi->vertices_vtx_buf != 0);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, gi->vertices_vtx_buf);
	glBufferData(GL_SHADER_STORAGE_BUFFER, vertex_bytes, vertices,
	    GL_DYNAMIC_DRAW);

	glGenBuffers(1, &gi->tails_vtx_buf);
	VERIFY(gi->tails_vtx_buf != 0);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, gi->tails_vtx_buf);
	glBufferData(GL_SHADER_STORAGE_BUFFER, tails_bytes, tails,
	    GL_DYNAMIC_DRAW);

	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

	free(droplets);
	free(vertices);
	free(tails);
}

static void
glass_info_init_compute_visual_droplets(glass_info_t *gi)
{
	size_t index_bytes = sizeof (GLuint) * gi->qual.num_droplets *
	    FACES_PER_DROPLET * 3;
	GLuint *indices = safe_malloc(index_bytes);

	VERIFY(gi->vertices_vtx_buf != 0);
	glGenBuffers(1, &gi->vertices_idx_buf);
	VERIFY(gi->vertices_idx_buf != 0);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gi->vertices_idx_buf);
	for (unsigned d = 0; d < gi->qual.num_droplets; d++) {
		GLuint *droplet = &indices[d * FACES_PER_DROPLET * 3];

		for (unsigned f = 0; f < FACES_PER_DROPLET; f++) {
			GLuint *face = &droplet[f * 3];

			face[0] = d * VTX_PER_DROPLET + f;
			face[1] = d * VTX_PER_DROPLET +
			    (f + 1) % FACES_PER_DROPLET;
			face[2] = d * VTX_PER_DROPLET + VTX_PER_DROPLET - 1;
		}
	}
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, index_bytes, indices,
	    GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	free(indices);
}

static void
glass_info_init_compute_visual_tails(glass_info_t *gi)
{
	size_t index_bytes = sizeof (GLuint) *
	    NUM_STREAMERS(gi->qual.num_droplets) * NUM_DROPLET_TAIL_SEGS * 2;
	GLuint *indices = safe_malloc(index_bytes);
	const GLuint *end = &indices[NUM_STREAMERS(gi->qual.num_droplets) *
	    NUM_DROPLET_TAIL_SEGS * 2];

	VERIFY(gi->tails_vtx_buf != 0);
	glGenBuffers(1, &gi->tails_idx_buf);
	VERIFY(gi->tails_idx_buf != 0);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gi->tails_idx_buf);
	for (unsigned d = 0; d < NUM_STREAMERS(gi->qual.num_droplets); d++) {
		GLuint *droplet = &indices[d * NUM_DROPLET_TAIL_SEGS * 2];
		GLuint p = d * NUM_DROPLET_HISTORY;

		for (unsigned t = 0; t < NUM_DROPLET_TAIL_SEGS; t++) {
			GLuint *seg = &droplet[t * 2];

			ASSERT3P(seg, <, end);
			seg[0] = p + t;
			seg[1] = p + t + 1;
			ASSERT3U(seg[0], <,
			    NUM_STREAMERS(gi->qual.num_droplets) *
			    NUM_DROPLET_HISTORY);
			ASSERT3U(seg[1], <,
			    NUM_STREAMERS(gi->qual.num_droplets) *
			    NUM_DROPLET_HISTORY);
		}
	}
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, index_bytes, indices,
	    GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	free(indices);
}

static void
glass_info_init_compute_visual(glass_info_t *gi)
{
	glass_info_init_compute_visual_droplets(gi);
	glass_info_init_compute_visual_tails(gi);
}

static void
glass_info_init_compute(glass_info_t *gi)
{
	glass_info_init_compute_phys(gi);
	glass_info_init_compute_visual(gi);
}

static void
init_glass_stencil(glass_info_t *gi, GLuint fbo, unsigned w, unsigned h)
{
	glutils_debug_push(0, "init_glass_stencil(%s, %d)",
	    GLASS_NAME(gi), fbo);

	glBindFramebufferEXT(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, w, h);

	glEnable(GL_STENCIL_TEST);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glStencilFunc(GL_NEVER, 1, 0xFF);
	glStencilOp(GL_REPLACE, GL_KEEP, GL_KEEP);

	glStencilMask(0xFF);
	glClear(GL_STENCIL_BUFFER_BIT);

	glUseProgram(stencil_init_prog);
	if (gi->glass->group_ids != NULL) {
		for (int i = 0; gi->glass->group_ids[i] != NULL; i++) {
			obj8_draw_group(gi->glass->obj, gi->glass->group_ids[i],
			    stencil_init_prog, GLM_MAT4_IDENTITY);
		}
	} else {
		obj8_draw_group(gi->glass->obj, NULL, stencil_init_prog,
		    GLM_MAT4_IDENTITY);
	}
	glUseProgram(0);

	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glStencilMask(0x00);
	glStencilFunc(GL_EQUAL, 1, 0xFF);
	glDisable(GL_STENCIL_TEST);

	glutils_debug_pop();
}

static void
glass_info_init_stencils(glass_info_t *gi)
{
	GLint old_fbo;
	GLint vp[4];

	old_fbo = librain_get_current_fbo();
	librain_get_current_vp(vp);

	for (int i = 0; i < 2; i++) {
		init_glass_stencil(gi, gi->water_depth_fbo[i],
		    DEPTH_TEX_SZ(gi), DEPTH_TEX_SZ(gi));
	}

	glBindFramebufferEXT(GL_FRAMEBUFFER, old_fbo);
	glViewport(vp[0], vp[1], vp[2], vp[3]);
}

static bool_t
glass_info_init(glass_info_t *gi, const librain_glass_t *glass)
{
	GLfloat *temp_tex;

	temp_tex = safe_calloc(sizeof (*temp_tex),
	    WS_TEMP_TEX_W * WS_TEMP_TEX_H);

	validate_glass(glass);

	gi->glass = glass;
	gi->qual = glass->qual;

	/* Apply some defaults */
	if (gi->qual.num_droplets == 0)
		gi->qual.num_droplets = 16384;

	if (!have_compute())
		gi->qual.use_compute = B_FALSE;
	else if (gi->qual.use_compute)
		VERIFY3U(gi->qual.num_droplets % DROPLET_WG_SIZE, ==, 0);

	glm_ortho(0, DEPTH_TEX_SZ(gi), 0, DEPTH_TEX_SZ(gi), 0, 1,
	    gi->water_depth_pvm);
	glm_ortho(0, NORM_TEX_SZ(gi), 0, NORM_TEX_SZ(gi),
	    0, 1, gi->water_norm_pvm);
	glm_ortho(0, WS_TEMP_TEX_W, 0, WS_TEMP_TEX_H, 0, 1, gi->ws_temp_pvm);

	/*
	 * Pre-stage: glass heating/cooling simulation.
	 */
	glGenTextures(2, gi->ws_temp_tex);
	glGenFramebuffers(2, gi->ws_temp_fbo);
	for (int i = 0; i < 2; i++) {
		setup_texture(gi->ws_temp_tex[i], GL_R32F, WS_TEMP_TEX_W,
		    WS_TEMP_TEX_H, GL_RED, GL_FLOAT, temp_tex);
		setup_color_fbo_for_tex(gi->ws_temp_fbo[i], gi->ws_temp_tex[i],
		    0, 0, B_FALSE);
		IF_TEXSZ(TEXSZ_ALLOC_INSTANCE(librain_ws_temp_tex, glass,
		    NULL, 0, GL_RED, GL_FLOAT, WS_TEMP_TEX_W, WS_TEMP_TEX_H));
	}

	/*
	 * Stage 1: computing water depth.
	 */
	glGenTextures(2, gi->water_depth_tex);
	glGenTextures(2, gi->water_depth_tex_stencil);
	glGenFramebuffers(2, gi->water_depth_fbo);
	for (int i = 0; i < 2; i++) {
		if (gi->qual.use_compute) {
			setup_texture(gi->water_depth_tex[i], GL_R8,
			    DEPTH_TEX_SZ(gi), DEPTH_TEX_SZ(gi), GL_RED,
			    GL_UNSIGNED_BYTE, NULL);
			IF_TEXSZ(TEXSZ_ALLOC_INSTANCE(librain_water_depth_tex,
			    glass, NULL, 0, GL_R8, GL_UNSIGNED_BYTE,
			    DEPTH_TEX_SZ(gi), DEPTH_TEX_SZ(gi)));
		} else {
			setup_texture(gi->water_depth_tex[i], GL_R16F,
			    DEPTH_TEX_SZ(gi), DEPTH_TEX_SZ(gi),
			    GL_RED, GL_FLOAT, NULL);
			IF_TEXSZ(TEXSZ_ALLOC_INSTANCE(librain_water_depth_tex,
			    glass, NULL, 0, GL_R16F, GL_FLOAT,
			    DEPTH_TEX_SZ(gi), DEPTH_TEX_SZ(gi)));
		}
		/*
		 * The stencil always matches the depth texture size exactly,
		 * so nearest filtering is good enough.
		 * Please note: on Apple we need to give the framebuffer a
		 * depth buffer as well to pass the framebuffer completeness
		 * check, so we use a combined 24/8 combined depth/stencil
		 * texture. We just ignore the depth texture.
		 */
#if	APL
		setup_texture_filter(gi->water_depth_tex_stencil[i], 1,
		    GL_DEPTH24_STENCIL8, DEPTH_TEX_SZ(gi), DEPTH_TEX_SZ(gi),
		    GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL, GL_NEAREST,
		    GL_NEAREST);
		setup_color_fbo_for_tex(gi->water_depth_fbo[i],
		    gi->water_depth_tex[i], gi->water_depth_tex_stencil[i],
		    gi->water_depth_tex_stencil[i], B_TRUE);
#else	/* !APL */
		setup_texture_filter(gi->water_depth_tex_stencil[i], 1,
		    GL_STENCIL_INDEX8, DEPTH_TEX_SZ(gi), DEPTH_TEX_SZ(gi),
		    GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, NULL, GL_NEAREST,
		    GL_NEAREST);
		setup_color_fbo_for_tex(gi->water_depth_fbo[i],
		    gi->water_depth_tex[i], 0, gi->water_depth_tex_stencil[i],
		    B_FALSE);
#endif	/* !APL */
		IF_TEXSZ(TEXSZ_ALLOC_INSTANCE(librain_water_depth_tex,
		    glass, NULL, 0, GL_STENCIL_INDEX8, GL_UNSIGNED_BYTE,
		    DEPTH_TEX_SZ(gi), DEPTH_TEX_SZ(gi)));
	}

	/*
	 * Stage 2: computing normals.
	 */
	glGenTextures(1, &gi->water_norm_tex);
	glGenFramebuffers(1, &gi->water_norm_fbo);

	setup_texture_filter(gi->water_norm_tex, MIPLEVELS, GL_RG8,
	    NORM_TEX_SZ(gi), NORM_TEX_SZ(gi), GL_RG, GL_UNSIGNED_BYTE, NULL,
	    GL_LINEAR, GL_LINEAR_MIPMAP_LINEAR);
	/*
	 * The stencil texture doesn't need to be mipmapped, as it's only
	 * ever used in native 1:1 texel mapping when constructing the
	 * normal map.
	 */
#if	APL
	setup_color_fbo_for_tex(gi->water_norm_fbo, gi->water_norm_tex,
	    0, 0, B_FALSE);
#else	/* !APL */
	setup_color_fbo_for_tex(gi->water_norm_fbo, gi->water_norm_tex,
	    0, 0, B_FALSE);
#endif	/* !APL */
	IF_TEXSZ(TEXSZ_ALLOC_INSTANCE(librain_water_norm_tex, glass, NULL, 0,
	    GL_RG, GL_UNSIGNED_BYTE, NORM_TEX_SZ(gi), NORM_TEX_SZ(gi)));
	IF_TEXSZ(TEXSZ_ALLOC_INSTANCE(librain_water_norm_tex, glass, NULL, 0,
	    GL_STENCIL_INDEX8, GL_UNSIGNED_BYTE, NORM_TEX_SZ(gi),
	    NORM_TEX_SZ(gi)));

	free(temp_tex);

	glass_info_init_quads(gi);

	for (int i = 0; i < MAX_WIPERS; i++)
		reset_wiper(&gi->wipers[i]);

	if (gi->qual.use_compute)
		glass_info_init_compute(gi);

	glass_info_init_stencils(gi);

	return (B_TRUE);
}

static void
glass_info_fini(glass_info_t *gi)
{
	for (int i = 0; i < 2; i++) {
		if (gi->ws_temp_tex[0] != 0) {
			IF_TEXSZ(TEXSZ_FREE_INSTANCE(librain_ws_temp_tex,
			    gi->glass, GL_RED, GL_FLOAT, WS_TEMP_TEX_W,
			    WS_TEMP_TEX_H));
		}
		if (gi->water_depth_tex[0] != 0) {
			if (gi->qual.use_compute) {
				IF_TEXSZ(TEXSZ_FREE_INSTANCE(
				    librain_water_depth_tex, gi->glass, GL_R8,
				    GL_UNSIGNED_BYTE, DEPTH_TEX_SZ(gi),
				    DEPTH_TEX_SZ(gi)));
			} else {
				IF_TEXSZ(TEXSZ_FREE_INSTANCE(
				    librain_water_depth_tex, gi->glass,
				    GL_R16F, GL_FLOAT, DEPTH_TEX_SZ(gi),
				    DEPTH_TEX_SZ(gi)));
			}
			IF_TEXSZ(TEXSZ_FREE_INSTANCE(librain_water_depth_tex,
			    gi->glass, GL_STENCIL_INDEX8, GL_UNSIGNED_BYTE,
			    DEPTH_TEX_SZ(gi), DEPTH_TEX_SZ(gi)));
		}
	}
	if (gi->water_norm_tex != 0) {
		IF_TEXSZ(TEXSZ_FREE_INSTANCE(librain_water_norm_tex,
		    gi->glass, GL_RG, GL_UNSIGNED_BYTE,
		    NORM_TEX_SZ(gi), NORM_TEX_SZ(gi)));
		IF_TEXSZ(TEXSZ_FREE_INSTANCE(librain_water_norm_tex,
		    gi->glass, GL_STENCIL_INDEX8, GL_UNSIGNED_BYTE,
		    NORM_TEX_SZ(gi), NORM_TEX_SZ(gi)));
	}

	DESTROY_OP(gi->ws_temp_fbo[0], 0,
	    glDeleteFramebuffers(2, gi->ws_temp_fbo));

	DESTROY_OP(gi->ws_temp_tex[0], 0,
	    glDeleteTextures(2, gi->ws_temp_tex));
	glutils_destroy_quads(&gi->ws_temp_quads);

	DESTROY_OP(gi->water_depth_fbo[0], 0,
	    glDeleteFramebuffers(2, gi->water_depth_fbo));
	DESTROY_OP(gi->water_depth_tex[0], 0,
	    glDeleteTextures(2, gi->water_depth_tex));
	DESTROY_OP(gi->water_depth_tex_stencil[0], 0,
	    glDeleteTextures(2, gi->water_depth_tex_stencil));
	glutils_destroy_quads(&gi->water_depth_quads);

	DESTROY_OP(gi->water_norm_fbo, 0,
	    glDeleteFramebuffers(1, &gi->water_norm_fbo));
	DESTROY_OP(gi->water_norm_tex, 0,
	    glDeleteTextures(1, &gi->water_norm_tex));
	glutils_destroy_quads(&gi->water_norm_quads);

	DESTROY_OP(gi->droplets_ssbo, 0,
	    glDeleteBuffers(1, &gi->droplets_ssbo));

	DESTROY_OP(gi->vertices_vtx_buf, 0,
	    glDeleteBuffers(1, &gi->vertices_vtx_buf));
	DESTROY_OP(gi->vertices_idx_buf, 0,
	    glDeleteBuffers(1, &gi->vertices_idx_buf));

	DESTROY_OP(gi->tails_vtx_buf, 0,
	    glDeleteBuffers(1, &gi->tails_vtx_buf));
	DESTROY_OP(gi->tails_idx_buf, 0,
	    glDeleteBuffers(1, &gi->tails_idx_buf));
}

static bool_t
water_effects_init(const librain_glass_t *glass, size_t num)
{
	glass_infos = safe_calloc(sizeof (*glass_infos), num);
	num_glass_infos = num;

	for (size_t i = 0; i < num_glass_infos; i++) {
		if (!glass_info_init(&glass_infos[i], &glass[i]))
			return (B_FALSE);
	}

	return (B_TRUE);
}

static void
water_effects_fini(void)
{
	DESTROY_OP(z_depth_prog, 0, glDeleteProgram(z_depth_prog));
	DESTROY_OP(stencil_init_prog, 0, glDeleteProgram(stencil_init_prog));
	DESTROY_OP(ws_temp_prog, 0, glDeleteProgram(ws_temp_prog));
	DESTROY_OP(rain_stage1_prog, 0, glDeleteProgram(rain_stage1_prog));
	DESTROY_OP(rain_stage2_prog, 0, glDeleteProgram(rain_stage2_prog));
	DESTROY_OP(rain_stage2_comp_prog, 0,
	    glDeleteProgram(rain_stage2_comp_prog));
	DESTROY_OP(ws_rain_prog, 0, glDeleteProgram(ws_rain_prog));
	DESTROY_OP(ws_rain_comp_prog, 0, glDeleteProgram(ws_rain_comp_prog));
	DESTROY_OP(ws_smudge_prog, 0, glDeleteProgram(ws_smudge_prog));
	DESTROY_OP(ws_smudge_comp_prog, 0,
	    glDeleteProgram(ws_smudge_comp_prog));
	DESTROY_OP(droplets_prog, 0, glDeleteProgram(droplets_prog));
	DESTROY_OP(droplets_paint_prog, 0,
	    glDeleteProgram(droplets_paint_prog));
	DESTROY_OP(tails_prog, 0, glDeleteProgram(tails_prog));

	for (size_t i = 0; i < num_glass_infos; i++)
		glass_info_fini(&glass_infos[i]);

	free(glass_infos);
	glass_infos = NULL;
	num_glass_infos = 0;
}

static void
ws_temp_comp_loc_resolve(GLuint prog, ws_temp_prog_loc_t *loc)
{
	loc->pvm = glGetUniformLocation(prog, "pvm");
	loc->src = glGetUniformLocation(prog, "src");
	loc->depth = glGetUniformLocation(prog, "depth");
	loc->rand_seed = glGetUniformLocation(prog, "rand_seed");
	loc->le_temp = glGetUniformLocation(prog, "le_temp");
	loc->cabin_temp = glGetUniformLocation(prog, "cabin_temp");
	loc->wind_fact = glGetUniformLocation(prog, "wind_fact");
	loc->d_t = glGetUniformLocation(prog, "d_t");
	loc->inertia_in = glGetUniformLocation(prog, "inertia_in");
	loc->heat_zones = glGetUniformLocation(prog, "heat_zones");
	loc->heat_tgt_temps = glGetUniformLocation(prog, "heat_tgt_temps");
	loc->precip_intens = glGetUniformLocation(prog, "precip_intens");
	loc->hot_air_src = glGetUniformLocation(prog, "hot_air_src");
	loc->hot_air_radius = glGetUniformLocation(prog, "hot_air_radius");
	loc->hot_air_temp = glGetUniformLocation(prog, "hot_air_temp");
}

static void
stage1_prog_loc_resolve(GLuint prog, rain_stage1_loc_t *loc)
{
	loc->pvm = glGetUniformLocation(prog, "pvm");
	loc->tex = glGetUniformLocation(prog, "tex");
	loc->temp_tex = glGetUniformLocation(prog, "temp_tex");
	loc->rand_seed = glGetUniformLocation(prog, "rand_seed");
	loc->precip_intens = glGetUniformLocation(prog, "precip_intens");
	loc->thrust = glGetUniformLocation(prog, "thrust");
	loc->wind = glGetUniformLocation(prog, "wind");
	loc->gravity = glGetUniformLocation(prog, "gravity");
	loc->d_t = glGetUniformLocation(prog, "d_t");
	loc->gp = glGetUniformLocation(prog, "gp");
	loc->tp = glGetUniformLocation(prog, "tp");
	loc->wp = glGetUniformLocation(prog, "wp");
	loc->wind_temp = glGetUniformLocation(prog, "wind_temp");
}

static void
droplets_paint_loc_resolve(droplets_paint_loc_t *loc, GLuint prog)
{
	ASSERT(loc != NULL);
	ASSERT(prog != 0);

	loc->pos = glGetAttribLocation(prog, "pos");
	loc->ctr = glGetAttribLocation(prog, "ctr");
	loc->radius = glGetAttribLocation(prog, "radius");
	loc->size = glGetAttribLocation(prog, "size");
}

static void
droplets_move_loc_resolve(droplets_move_loc_t *loc, GLuint prog)
{
	loc->ws_temp_tex = glGetUniformLocation(prog, "ws_temp_tex");
	loc->cur_t = glGetUniformLocation(prog, "cur_t");
	loc->d_t = glGetUniformLocation(prog, "d_t");
	loc->rand_seed = glGetUniformLocation(prog, "rand_seed");
	loc->gravity_point = glGetUniformLocation(prog, "gravity_point");
	loc->gravity_force = glGetUniformLocation(prog, "gravity_force");
	loc->wind_point = glGetUniformLocation(prog, "wind_point");
	loc->wind_force = glGetUniformLocation(prog, "wind_force");
	loc->thrust_point = glGetUniformLocation(prog, "thrust_point");
	loc->thrust_force = glGetUniformLocation(prog, "thrust_force");
	loc->precip_intens = glGetUniformLocation(prog, "precip_intens");
	loc->min_droplet_sz = glGetUniformLocation(prog, "min_droplet_sz");
	loc->le_temp = glGetUniformLocation(prog, "le_temp");
}

static void
stage2_loc_resolve(stage2_loc_t *loc, GLuint prog)
{
	ASSERT(loc != NULL);
	ASSERT(prog != 0);

	loc->pvm = glGetUniformLocation(prog, "pvm");
	loc->tex = glGetUniformLocation(prog, "tex");
	loc->temp_tex = glGetUniformLocation(prog, "temp_tex");
	loc->tp = glGetUniformLocation(prog, "tp");
	loc->thrust = glGetUniformLocation(prog, "thrust");
	loc->wp = glGetUniformLocation(prog, "wp");
	loc->wind = glGetUniformLocation(prog, "wind");
	loc->precip_intens = glGetUniformLocation(prog, "precip_intens");
	loc->window_ice = glGetUniformLocation(prog, "window_ice");
}

bool_t
librain_reload_gl_progs(void)
{
	if (have_compute()) {
		if (!reload_gl_prog(&rain_stage2_comp_prog,
		    &rain_stage2_comp_prog_info) ||
		    !reload_gl_prog(&ws_rain_comp_prog,
		    &ws_rain_comp_prog_info) ||
		    !reload_gl_prog(&droplets_prog, &droplets_prog_info) ||
		    !reload_gl_prog(&droplets_paint_prog,
		    &droplets_paint_prog_info) ||
		    !reload_gl_prog(&tails_prog, &tails_prog_info))
			return (B_FALSE);

		droplets_move_loc_resolve(&droplets_move_loc, droplets_prog);
		droplets_paint_loc_resolve(&droplets_paint_prog_loc,
		    droplets_paint_prog);

		tails_prog_loc.pos = glGetAttribLocation(tails_prog, "pos");
		tails_prog_loc.quant = glGetAttribLocation(tails_prog, "quant");

		stage2_loc_resolve(&stage2_comp_loc, rain_stage2_comp_prog);
	}

	if (!reload_gl_prog(&z_depth_prog, &z_depth_prog_info) ||
	    !reload_gl_prog(&stencil_init_prog, &stencil_init_prog_info) ||
	    !reload_gl_prog(&ws_temp_prog, &ws_temp_prog_info) ||
	    !reload_gl_prog(&rain_stage1_prog, &rain_stage1_prog_info) ||
	    !reload_gl_prog(&rain_stage2_prog, &rain_stage2_prog_info) ||
	    !reload_gl_prog(&ws_rain_prog, &ws_rain_prog_info) ||
	    !reload_gl_prog(&ws_smudge_prog, &ws_smudge_prog_info) ||
	    !reload_gl_prog(&ws_smudge_comp_prog, &ws_smudge_comp_prog_info)) {
		return (B_FALSE);
	}

	stage2_loc_resolve(&stage2_loc, rain_stage2_prog);
	ws_temp_comp_loc_resolve(ws_temp_prog, &ws_temp_prog_loc);
	stage1_prog_loc_resolve(rain_stage1_prog, &rain_stage1_loc);

	return (B_TRUE);
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

	XPLMGetVersions(&xp_ver, &xplm_ver, &host_id);

	return (B_TRUE);
}

static void
setup_smudge_tex(void)
{
	ASSERT0(ws_smudge_tex);
	ASSERT0(ws_smudge_fbo);

	glGenTextures(1, &ws_smudge_tex);
	glGenFramebuffers(1, &ws_smudge_fbo);
	setup_texture(ws_smudge_tex, GL_RGBA8, ss_texsz[0], ss_texsz[1],
	    GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	setup_color_fbo_for_tex(ws_smudge_fbo, ws_smudge_tex, 0, 0, B_FALSE);
	IF_TEXSZ(TEXSZ_ALLOC(librain_ws_smudge_tex, GL_RGBA, GL_UNSIGNED_BYTE,
	    ss_texsz[0], ss_texsz[1]));

	GLUTILS_ASSERT_NO_ERROR();
}

static void
destroy_smudge_tex(void)
{
	if (ws_smudge_tex != 0) {
		IF_TEXSZ(TEXSZ_FREE(librain_ws_smudge_tex, GL_RGBA,
		    GL_UNSIGNED_BYTE, ss_texsz[0], ss_texsz[1]));
	}
	DESTROY_OP(ws_smudge_fbo, 0, glDeleteFramebuffers(1, &ws_smudge_fbo));
	DESTROY_OP(ws_smudge_tex, 0, glDeleteTextures(1, &ws_smudge_tex));
}

/*
 * Initializes librain for operation. This MUST be called at plugin load
 * time.
 */
bool_t
librain_init(const char *the_shaderpath, const librain_glass_t *glass,
    size_t num)
{
	GLint old_fbo;

	ASSERT(the_shaderpath != NULL);
	ASSERT(glass != NULL);
	ASSERT(num != 0);

	ASSERT_MSG(!inited, "Multiple calls to librain_init() detected. "
	    "De-initialize the library first using librain_fini().%s", "");
	inited = B_TRUE;
	GLUTILS_RESET_ERRORS();

	if (!librain_glob_init())
		return (B_FALSE);

	old_fbo = librain_get_current_fbo();
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	shaderpath = strdup(the_shaderpath);

	memset(&drs, 0, sizeof (drs));

	fdr_find(&drs.panel_render_type, "sim/graphics/view/panel_render_type");
	fdr_find(&drs.plane_render_type, "sim/graphics/view/plane_render_type");
	fdr_find(&drs.world_render_type, "sim/graphics/view/world_render_type");
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
	drs.VR_enabled_avail =
	    dr_find(&drs.VR_enabled, "sim/graphics/VR/enabled");
	fdr_find(&drs.draw_call_type, "sim/graphics/view/draw_call_type");
	drs.rev_float_z_avail =
	    dr_find(&drs.rev_float_z, "sim/graphics/view/is_reverse_float_z");
	drs.rev_y_avail =
	    dr_find(&drs.rev_y, "sim/graphics/view/is_reverse_y");

	drs.aa_ratio_avail = (dr_find(&drs.fsaa_ratio_x,
	    "sim/private/controls/hdr/fsaa_ratio_x") &&
	    dr_find(&drs.fsaa_ratio_y,
	    "sim/private/controls/hdr/fsaa_ratio_y"));
	drs.current_gl_fbo_avail = dr_find(&drs.current_gl_fbo,
	    "sim/graphics/view/current_gl_fbo");
	drs.modern_driver_avail = dr_find(&drs.modern_driver,
	    "sim/graphics/view/using_modern_driver");

	drs.xe_present = (dr_find(&drs.xe_active, "env/active") &&
	    dr_find(&drs.xe_rain, "env/rain") &&
	    dr_find(&drs.xe_snow, "env/snow"));

	VERIFY(XPLMRegisterDrawCallback(rain_comp_cb, RAIN_COMP_PHASE,
	    RAIN_COMP_BEFORE, NULL));
	VERIFY(XPLMRegisterDrawCallback(rain_paint_cb, RAIN_PAINT_PHASE,
	    RAIN_PAINT_BEFORE, NULL));
	if (using_modern_driver()) {
		VERIFY(XPLMRegisterDrawCallback(capture_mtx,
		    MTX_CAPTURE_PHASE_MODERN, MTX_CAPTURE_BEFORE_MODERN, NULL));
	} else {
		VERIFY(XPLMRegisterDrawCallback(capture_mtx,
		    MTX_CAPTURE_PHASE_LEGACY, MTX_CAPTURE_BEFORE_LEGACY, NULL));
	}

	if (!GLEW_ARB_framebuffer_object) {
		logMsg("Cannot initialize: your OpenGL version doesn't "
		    "appear to support ARB_framebuffer_object");
		goto errout;
	}

	glGenTextures(1, &screenshot_tex);
	glBindTexture(GL_TEXTURE_2D, screenshot_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, ss_texsz[0], ss_texsz[1],
	    0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
	IF_TEXSZ(TEXSZ_ALLOC(librain_screenshot_tex, GL_RGB, GL_UNSIGNED_BYTE,
	    ss_texsz[0], ss_texsz[1]));

	glGenFramebuffers(1, &screenshot_fbo);
	glBindFramebufferEXT(GL_FRAMEBUFFER, screenshot_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_TEXTURE_2D, screenshot_tex, 0);
	VERIFY3U(glCheckFramebufferStatus(GL_FRAMEBUFFER), ==,
	    GL_FRAMEBUFFER_COMPLETE);

	/*
	 * Final render pre-stage: capture the full res displacement,
	 * which is then used as the final smudge pass.
	 */
	setup_smudge_tex();

	/* Must go ahead of VAO construction */
	if (!librain_reload_gl_progs())
		goto errout;

	if (!water_effects_init(glass, num))
		goto errout;

	reinit.glass = glass;
	reinit.num = num;

	glBindFramebufferEXT(GL_FRAMEBUFFER, old_fbo);
	gl_state_cleanup();

	return (B_TRUE);
errout:
	librain_fini();

	glBindFramebufferEXT(GL_FRAMEBUFFER, old_fbo);
	gl_state_cleanup();

	return (B_FALSE);
}

void
librain_fini(void)
{
	if (!inited)
		return;
	GLUTILS_RESET_ERRORS();

	free(shaderpath);
	shaderpath = NULL;

	if (using_modern_driver()) {
		XPLMUnregisterDrawCallback(capture_mtx,
		    MTX_CAPTURE_PHASE_MODERN, MTX_CAPTURE_BEFORE_MODERN, NULL);
	} else {
		XPLMUnregisterDrawCallback(capture_mtx,
		    MTX_CAPTURE_PHASE_LEGACY, MTX_CAPTURE_BEFORE_LEGACY, NULL);
	}
	XPLMUnregisterDrawCallback(rain_paint_cb, RAIN_PAINT_PHASE,
	    RAIN_PAINT_BEFORE, NULL);
	XPLMUnregisterDrawCallback(rain_comp_cb, RAIN_COMP_PHASE,
	    RAIN_COMP_BEFORE, NULL);

	if (screenshot_tex != 0) {
		IF_TEXSZ(TEXSZ_FREE(librain_screenshot_tex, GL_RGB,
		    GL_UNSIGNED_BYTE, ss_texsz[0], ss_texsz[1]));
	}
	DESTROY_OP(screenshot_fbo, 0, glDeleteFramebuffers(1, &screenshot_fbo));
	DESTROY_OP(screenshot_tex, 0, glDeleteTextures(1, &screenshot_tex));

	destroy_smudge_tex();

	DESTROY_OP(priv_depth_tex, 0, glDeleteTextures(1, &priv_depth_tex));
	priv_depth_tex_w = 0;
	priv_depth_tex_h = 0;

	water_effects_fini();

	for (int i = 2; i < 4; i++)
		cur_vp[i] = DFL_VP_SIZE;
	ss_texsz[0] = DFL_VP_SIZE;
	ss_texsz[1] = DFL_VP_SIZE;

	GLUTILS_ASSERT_NO_ERROR();

	inited = B_FALSE;
}

void
librain_get_pvm(mat4 pvm)
{
	check_librain_init();
	memcpy(pvm, glob_pvm, sizeof (mat4));
}

void
librain_get_vp(vec4 vp)
{
	check_librain_init();
	memcpy(vp, glob_vp, sizeof (vec4));
}

GLuint
librain_get_screenshot_tex(void)
{
	check_librain_init();
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
	check_librain_init();
	debug_draw = flag;
}

/*
 * By setting this flag to true, the library will draw a visible outline
 * around the wiper area and where the wipers are located. This can be
 * used for fine-tuning the wiper position constants.
 */
LIBRAIN_EXPORT void
librain_set_wipers_visible(bool_t flag)
{
	check_librain_init();
	wipers_visible = flag;
}
