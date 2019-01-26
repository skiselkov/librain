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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include <XPLMDisplay.h>
#include <XPLMPlugin.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include <acfutils/crc64.h>
#include <acfutils/glutils.h>
#include <acfutils/helpers.h>
#include <acfutils/perf.h>
#include <acfutils/time.h>

#include "../src/librain.h"
#include "../src/obj8.h"
#include "../src/surf_ice.h"

#define	PLUGIN_NAME		"librain"
#define	PLUGIN_SIG		"skiselkov.librain"
#define	PLUGIN_DESCRIPTION	"librain (DO NOT DISABLE)"

typedef struct {
	obj8_t		*obj;
	vect3_t		pos_offset;
	char		filename[512];
	bool_t		load;
	bool_t		loaded;

	struct {
		dr_t	filename;
		dr_t	pos_offset[3];
		dr_t	load;
		dr_t	loaded;
	} drs;
} obj_data_t;

typedef struct {
	obj_data_t	obj_data;

	librain_glass_t	*glass;

	double		wiper_angle[MAX_WIPERS];
	bool_t		wiper_moving[MAX_WIPERS];

	struct {
		dr_t	slant_factor;

		dr_t	thrust_point_x;
		dr_t	thrust_point_y;
		dr_t	thrust_factor;
		dr_t	max_thrust;

		dr_t	gravity_point_x;
		dr_t	gravity_point_y;
		dr_t	gravity_factor;

		dr_t	wind_point_x;
		dr_t	wind_point_y;
		dr_t	wind_factor;
		dr_t	wind_normal;

		dr_t	max_tas;

		dr_t	therm_inertia;
		dr_t	cabin_temp;
		dr_t	heat_zones;
		dr_t	heat_tgt_temps;

		dr_t	hot_air_src;
		dr_t	hot_air_radius;
		dr_t	hot_air_temp;

		dr_t	wiper_pivot_x[MAX_WIPERS];
		dr_t	wiper_pivot_y[MAX_WIPERS];
		dr_t	wiper_radius_outer[MAX_WIPERS];
		dr_t	wiper_radius_inner[MAX_WIPERS];
		dr_t	wiper_angle[MAX_WIPERS];
		dr_t	wiper_moving[MAX_WIPERS];
	} drs;
} glass_data_t;

static char xpdir[512];
static char plugindir[512];

#define	MAX_GLASS		4
#define	MAX_Z_DEPTH_OBJS	20

static librain_glass_t	glass_info[MAX_GLASS];
static glass_data_t	glass_data[MAX_GLASS];
static obj_data_t	z_depth_objs[MAX_Z_DEPTH_OBJS];

static bool_t		librain_do_init = B_FALSE;
static bool_t		librain_inited = B_FALSE;
static int		num_glass_use = 0;
static bool_t		verbose = B_FALSE;
static bool_t		debug_draw = B_FALSE;
static bool_t		wipers_visible = B_FALSE;

static struct {
	dr_t		librain_do_init;
	dr_t		librain_inited;
	dr_t		num_glass_use;
	dr_t		verbose;
	dr_t		debug_draw;
	dr_t		wipers_visible;
} drs;

static void
wiper_cb(dr_t *dr, void *value_p)
{
	UNUSED(dr);
	ASSERT(value_p != NULL);
	wipers_visible = !!(*(int *)value_p);
	if (librain_inited)
		librain_set_wipers_visible(wipers_visible);
}

static void
debug_draw_cb(dr_t *dr, void *value_p)
{
	UNUSED(dr);
	ASSERT(value_p != NULL);
	*(int *)value_p = !!(*(int *)value_p);
	if (librain_inited)
		librain_set_debug_draw(*(int *)value_p);
}

static void
load_obj_cb(dr_t *dr, void *value_p)
{
	int value;
	obj_data_t *od;

	ASSERT(dr != NULL);
	ASSERT(dr->cb_userinfo != NULL);
	ASSERT(value_p != NULL);

	od = dr->cb_userinfo;
	value = *(int *)value_p;

	if (od->obj != NULL) {
		if (verbose) {
			logMsg("unloading object %s",
			    obj8_get_filename(od->obj));
		}
		obj8_free(od->obj);
		od->obj = NULL;
		od->loaded = B_FALSE;
	}
	if (value != 0 && strlen(od->filename) > 0) {
		od->obj = obj8_parse(od->filename, od->pos_offset);
		od->loaded = (od->obj != NULL);
		if (od->loaded && verbose)
			logMsg("loaded object %s", obj8_get_filename(od->obj));
	}
}

static bool_t
pre_init_validate(void)
{
	if (num_glass_use == 0) {
		logMsg("librain init error: first populate all required "
		    "librain/glass[*] instances and set librain/num_glass_use "
		    "to the number of glass surfaces you have defined. Only "
		    "then set librain/initialize = 1.");
		return (B_FALSE);
	}
	for (int i = 0; i < num_glass_use; i++) {
		if (glass_data[i].obj_data.obj == NULL) {
			logMsg("librain init error: you have set "
			    "librain/num_glass_use = %d, but "
			    "librain/glass_%d/obj is not loaded.",
			    num_glass_use, i);
			return (B_FALSE);
		}
		glass_info[i].obj = glass_data[i].obj_data.obj;
	}

	return (B_TRUE);
}

static void
librain_init_cb(dr_t *dr, void *value_p)
{
	bool_t value;

	UNUSED(dr);
	ASSERT(value_p != NULL);

	value = *(int *)value_p;
	/* Make sure the value is 1 or 0 */
	value = !!value;

	if (value == librain_inited) {
		*(int *)value_p = value;
		return;
	}

	if (!librain_inited) {
		char *shader_path;

		if (!pre_init_validate()) {
			*(int *)value_p = 0;
			return;
		}

		shader_path = mkpathname(xpdir, plugindir, "shaders", NULL);
		if (librain_init(shader_path, glass_info, num_glass_use)) {
			if (verbose)
				logMsg("librain_init success");
			librain_set_debug_draw(debug_draw);
			librain_set_wipers_visible(wipers_visible);
		} else {
			value = B_FALSE;
		}

		lacf_free(shader_path);
	} else {
		librain_fini();
		if (verbose)
			logMsg("librain_fini success");
	}

	*(int *)value_p = value;
	librain_inited = value;
}

static void
num_glass_write_cb(dr_t *dr, void *value_vp)
{
	int *value_p;

	UNUSED(dr);
	ASSERT(value_vp != NULL);
	value_p = value_vp;
	/* Make sure the value is properly clamped */
	*value_p = clampi(*value_p, 0, MAX_GLASS);
}

#define	VALIDATOR(name, min_val, max_val) \
static void \
name ## _validator(dr_t *dr, void *value_vp) \
{ \
	float *value_p; \
	UNUSED(dr); \
	ASSERT(value_vp != NULL); \
	value_p = value_vp; \
	if (*value_p < (min_val) || *value_p > (max_val)) { \
		logMsg("librain data error: " #name " must be a greater than " \
		    #min_val " and less than " #max_val); \
	} \
	*value_p = clamp(*value_p, (min_val), (max_val)); \
}

VALIDATOR(therm_inertia, 0.1, 10000)
VALIDATOR(max_tas, 10, 1000)
VALIDATOR(max_thrust, 0, 10000000)
VALIDATOR(angle, -M_PI, M_PI)
VALIDATOR(cabin_temp, 1, 1000)

static void
prepare_system_paths(void)
{
	char *p;

	XPLMGetSystemPath(xpdir);
	XPLMGetPluginInfo(XPLMGetMyID(), NULL, plugindir, NULL, NULL);

#if	IBM
	fix_pathsep(xpdir);
	fix_pathsep(plugindir);
#endif	/* IBM */

	/* cut off the trailing path component (our filename) */
	if ((p = strrchr(plugindir, DIRSEP)) != NULL)
		*p = '\0';
	/* cut off an optional '32' or '64' trailing component */
	if ((p = strrchr(plugindir, DIRSEP)) != NULL) {
		if (strcmp(p + 1, "64") == 0 || strcmp(p + 1, "32") == 0 ||
		    strcmp(p + 1, "win_x64") == 0 ||
		    strcmp(p + 1, "mac_x64") == 0 ||
		    strcmp(p + 1, "lin_x64") == 0)
			*p = '\0';
	}
	/*
	 * Now we strip a leading xpdir from plugindir, so that now plugindir
	 * will be relative to X-Plane's root directory.
	 */
	if (strstr(plugindir, xpdir) == plugindir) {
		int xpdir_len = strlen(xpdir);
		int plugindir_len = strlen(plugindir);

		memmove(plugindir, &plugindir[xpdir_len],
		    plugindir_len - xpdir_len + 1);
	}
}

static void
obj_data_init(obj_data_t *od, const char *prefix)
{
	memset(od, 0, sizeof (*od));
	dr_create_b(&od->drs.filename, od->filename, sizeof (od->filename),
	    B_TRUE, "%s/filename", prefix);
	dr_create_f64(&od->drs.pos_offset[0], &od->pos_offset.x, B_TRUE,
	    "%s/pos_offset/x", prefix);
	dr_create_f64(&od->drs.pos_offset[1], &od->pos_offset.y, B_TRUE,
	    "%s/pos_offset/y", prefix);
	dr_create_f64(&od->drs.pos_offset[2], &od->pos_offset.z, B_TRUE,
	    "%s/pos_offset/z", prefix);
	dr_create_i(&od->drs.load, (int *)&od->load, B_TRUE, "%s/load", prefix);
	dr_create_i(&od->drs.loaded, (int *)&od->loaded, B_FALSE,
	    "%s/loaded", prefix);
	od->drs.load.write_cb = load_obj_cb;
	od->drs.load.cb_userinfo = od;
}

static void
obj_data_fini(obj_data_t *od)
{
	if (od->obj != NULL)
		obj8_free(od->obj);
	dr_delete(&od->drs.filename);
	for (int i = 0; i < 3; i++)
		dr_delete(&od->drs.pos_offset[i]);
	dr_delete(&od->drs.load);
	dr_delete(&od->drs.loaded);
	memset(od, 0, sizeof (*od));
}

static void
glass_data_init(unsigned glass_i)
{
	char prefix[64];
	glass_data_t *gd;
	librain_glass_t *glass;

	ASSERT3U(glass_i, <, MAX_GLASS);
	gd = &glass_data[glass_i];
	glass = &glass_info[glass_i];

	memset(gd, 0, sizeof (*gd));
	memset(glass, 0, sizeof (*glass));

	gd->glass = glass;

	snprintf(prefix, sizeof (prefix), "librain/glass_%d/obj", glass_i);
	obj_data_init(&gd->obj_data, prefix);

	dr_create_f64(&gd->drs.slant_factor, &glass->slant_factor, B_TRUE,
	    "librain/glass_%d/slant_factor", glass_i);

	dr_create_f64(&gd->drs.thrust_point_x, &glass->thrust_point.x, B_TRUE,
	    "librain/glass_%d/thrust_point/x", glass_i);
	dr_create_f64(&gd->drs.thrust_point_y, &glass->thrust_point.y, B_TRUE,
	    "librain/glass_%d/thrust_point/y", glass_i);
	dr_create_f64(&gd->drs.thrust_factor, &glass->thrust_factor, B_TRUE,
	    "librain/glass_%d/thrust_factor", glass_i);
	dr_create_f64(&gd->drs.max_thrust, &glass->max_thrust, B_TRUE,
	    "librain/glass_%d/max_thrust", glass_i);
	gd->drs.max_thrust.write_cb = max_thrust_validator;

	dr_create_f64(&gd->drs.gravity_point_x, &glass->gravity_point.x, B_TRUE,
	    "librain/glass_%d/gravity_point/x", glass_i);
	dr_create_f64(&gd->drs.gravity_point_y, &glass->gravity_point.y, B_TRUE,
	    "librain/glass_%d/gravity_point/y", glass_i);
	dr_create_f64(&gd->drs.gravity_factor, &glass->gravity_factor, B_TRUE,
	    "librain/glass_%d/gravity_factor", glass_i);

	dr_create_f64(&gd->drs.wind_point_x, &glass->wind_point.x, B_TRUE,
	    "librain/glass_%d/wind_point/x", glass_i);
	dr_create_f64(&gd->drs.wind_point_y, &glass->wind_point.y, B_TRUE,
	    "librain/glass_%d/wind_point/y", glass_i);
	dr_create_f64(&gd->drs.wind_factor, &glass->wind_factor, B_TRUE,
	    "librain/glass_%d/wind_factor", glass_i);
	dr_create_f64(&gd->drs.wind_normal, &glass->wind_normal, B_TRUE,
	    "librain/glass_%d/wind_normal", glass_i);

	dr_create_f64(&gd->drs.max_tas, &glass->max_tas, B_TRUE,
	    "librain/glass_%d/max_tas", glass_i);
	gd->drs.max_tas.write_cb = max_tas_validator;

	dr_create_f(&gd->drs.therm_inertia, &glass->therm_inertia, B_TRUE,
	    "librain/glass_%d/therm_inertia", glass_i);
	gd->drs.therm_inertia.write_cb = therm_inertia_validator;

	dr_create_vf(&gd->drs.heat_zones, glass->heat_zones,
	    sizeof (glass->heat_zones) / sizeof (float), B_TRUE,
	    "librain/glass_%d/heat_zones", glass_i);
	dr_create_vf(&gd->drs.heat_tgt_temps, glass->heat_tgt_temps,
	    sizeof (glass->heat_tgt_temps) / sizeof (float), B_TRUE,
	    "librain/glass_%d/heat_tgt_temps", glass_i);
	dr_create_f(&gd->drs.cabin_temp, &glass->cabin_temp, B_TRUE,
	    "librain/glass_%d/cabin_temp", glass_i);
	gd->drs.cabin_temp.write_cb = cabin_temp_validator;

	dr_create_vf(&gd->drs.hot_air_src, glass->hot_air_src,
	    sizeof (glass->hot_air_src) / sizeof (float), B_TRUE,
	    "librain/glass_%d/hot_air_src", glass_i);
	dr_create_vf(&gd->drs.hot_air_radius, glass->hot_air_radius,
	    sizeof (glass->hot_air_radius) / sizeof (float), B_TRUE,
	    "librain/glass_%d/hot_air_radius", glass_i);
	dr_create_vf(&gd->drs.hot_air_temp, glass->hot_air_temp,
	    sizeof (glass->hot_air_temp) / sizeof (float), B_TRUE,
	    "librain/glass_%d/hot_air_temp", glass_i);

	for (int i = 0; i < MAX_WIPERS; i++) {
		dr_create_f64(&gd->drs.wiper_pivot_x[i],
		    &glass->wiper_pivot[i].x, B_TRUE,
		    "librain/glass_%d/wiper_%d/pivot/x", glass_i, i);
		dr_create_f64(&gd->drs.wiper_pivot_y[i],
		    &glass->wiper_pivot[i].y, B_TRUE,
		    "librain/glass_%d/wiper_%d/pivot/y", glass_i, i);
		dr_create_f64(&gd->drs.wiper_radius_outer[i],
		    &glass->wiper_radius_outer[i], B_TRUE,
		    "librain/glass_%d/wiper_%d/radius_outer", glass_i, i);
		dr_create_f64(&gd->drs.wiper_radius_inner[i],
		    &glass->wiper_radius_inner[i], B_TRUE,
		    "librain/glass_%d/wiper_%d/radius_inner", glass_i, i);
		dr_create_f64(&gd->drs.wiper_angle[i],
		    &gd->wiper_angle[i], B_TRUE,
		    "librain/glass_%d/wiper_%d/angle", glass_i, i);
		gd->drs.wiper_angle[i].write_cb = angle_validator;
		dr_create_i(&gd->drs.wiper_moving[i],
		    (int *)&gd->wiper_moving[i], B_TRUE,
		    "librain/glass_%d/wiper_%d/moving", glass_i, i);
	}

	/* sensible defaults */
	glass->max_tas = 100;
	glass->therm_inertia = 25;
	glass->cabin_temp = C2KELVIN(21);
	glass->slant_factor = 1;
	glass->wind_factor = 1;
	glass->wind_normal = 1;
	glass->wind_point = VECT2(0.5, -2);
	glass->gravity_factor = 0.25;
	glass->gravity_point = VECT2(0.5, 2);
	glass->thrust_point = VECT2(0.5, -2);
}

static void
glass_data_fini(unsigned glass_i)
{
	glass_data_t *gd;
	librain_glass_t *glass;

	ASSERT3U(glass_i, <, MAX_GLASS);
	gd = &glass_data[glass_i];
	ASSERT(gd->glass != NULL);
	/* We will be destroying the glass object, so it's ok to write to it */
	glass = (librain_glass_t *)gd->glass;

	for (int i = 0; i < MAX_WIPERS; i++) {
		dr_delete(&gd->drs.wiper_pivot_x[i]);
		dr_delete(&gd->drs.wiper_pivot_y[i]);
		dr_delete(&gd->drs.wiper_radius_outer[i]);
		dr_delete(&gd->drs.wiper_radius_inner[i]);
		dr_delete(&gd->drs.wiper_angle[i]);
		dr_delete(&gd->drs.wiper_moving[i]);
	}

	dr_delete(&gd->drs.hot_air_src);
	dr_delete(&gd->drs.hot_air_radius);
	dr_delete(&gd->drs.hot_air_temp);

	dr_delete(&gd->drs.cabin_temp);
	dr_delete(&gd->drs.heat_zones);
	dr_delete(&gd->drs.heat_tgt_temps);

	dr_delete(&gd->drs.therm_inertia);

	dr_delete(&gd->drs.max_tas);

	dr_delete(&gd->drs.wind_point_x);
	dr_delete(&gd->drs.wind_point_y);
	dr_delete(&gd->drs.wind_factor);
	dr_delete(&gd->drs.wind_normal);

	dr_delete(&gd->drs.gravity_point_x);
	dr_delete(&gd->drs.gravity_point_y);
	dr_delete(&gd->drs.gravity_factor);

	dr_delete(&gd->drs.thrust_point_x);
	dr_delete(&gd->drs.thrust_point_y);
	dr_delete(&gd->drs.thrust_factor);
	dr_delete(&gd->drs.max_thrust);

	dr_delete(&gd->drs.slant_factor);

	obj_data_fini(&gd->obj_data);

	if (gd->glass->group_ids != NULL) {
		for (int i = 0; gd->glass->group_ids[i] != NULL; i++)
			free((void *)(glass->group_ids[i]));
		free((void *)(glass->group_ids));
	}

	memset(gd->glass, 0, sizeof (*gd->glass));
	memset(gd, 0, sizeof (*gd));
}

static float
wiper_floop(float delta_t, float time2, int counter, void *refcon)
{
	UNUSED(delta_t);
	UNUSED(time2);
	UNUSED(counter);
	UNUSED(refcon);

	if (!librain_inited)
		return (1);

	for (int i = 0; i < num_glass_use; i++) {
		for (int j = 0; j < MAX_WIPERS; j++) {
			librain_glass_t *glass = &glass_info[i];
			if (glass->wiper_radius_outer[j] !=
			    glass->wiper_radius_inner[j]) {
				librain_set_wiper_angle(glass, j,
				    glass_data[i].wiper_angle[j],
				    glass_data[i].wiper_moving[j]);
			}
		}
	}

	return (-1);
}

static int
draw_rain_effects(XPLMDrawingPhase phase, int before, void *refcon)
{
	UNUSED(phase);
	UNUSED(before);
	UNUSED(refcon);

	if (!librain_inited)
		return (1);

	librain_draw_prepare(B_TRUE);
	for (int i = 0; i < MAX_Z_DEPTH_OBJS; i++) {
		if (z_depth_objs[i].obj != NULL)
			librain_draw_z_depth(z_depth_objs[i].obj, NULL);
	}
	librain_draw_exec();
	librain_draw_finish();

	return (1);
}

static void
log_dbg_string(const char *str)
{
	XPLMDebugString(str);
}

PLUGIN_API int
XPluginStart(char *name, char *sig, char *desc)
{
	GLenum err;

	/* Always use Unix-native paths on the Mac! */
	XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);
	XPLMEnableFeature("XPLM_USE_NATIVE_WIDGET_WINDOWS", 1);

	log_init(log_dbg_string, "librain");
	glutils_texsz_init();
	crc64_init();
	crc64_srand(microclock() + clock());
	logMsg("This is librain (" PLUGIN_VERSION ") libacfutils-%s",
	    libacfutils_version);

	prepare_system_paths();

	strcpy(name, PLUGIN_NAME);
	strcpy(sig, PLUGIN_SIG);
	strcpy(desc, PLUGIN_DESCRIPTION);
	err = glewInit();
	if (err != GLEW_OK) {
		/* Problem: glewInit failed, something is seriously wrong. */
		logMsg("FATAL ERROR: cannot initialize libGLEW: %s",
		    glewGetErrorString(err));
		goto errout;
	}
	if (!GLEW_VERSION_2_1) {
		logMsg("FATAL ERROR: your system doesn't support OpenGL 2.1");
		goto errout;
	}

	for (int i = 0; i < MAX_GLASS; i++)
		glass_data_init(i);
	for (int i = 0; i < MAX_Z_DEPTH_OBJS; i++) {
		char prefix[64];

		snprintf(prefix, sizeof (prefix), "librain/z_depth_obj_%d", i);
		obj_data_init(&z_depth_objs[i], prefix);
	}

	dr_create_i(&drs.librain_do_init, (int *)&librain_do_init, B_TRUE,
	    "librain/initialize");
	drs.librain_do_init.write_cb = librain_init_cb;
	dr_create_i(&drs.librain_inited, (int *)&librain_inited, B_FALSE,
	    "librain/init_success");
	dr_create_i(&drs.num_glass_use, &num_glass_use, B_TRUE,
	    "librain/num_glass_use");
	drs.num_glass_use.write_cb = num_glass_write_cb;
	dr_create_i(&drs.verbose, (int *)&verbose, B_TRUE, "librain/verbose");
	dr_create_i(&drs.debug_draw, (int *)&debug_draw, B_TRUE,
	    "librain/debug_draw");
	drs.debug_draw.write_cb = debug_draw_cb;
	dr_create_i(&drs.wipers_visible, (int *)&wipers_visible, B_TRUE,
	    "librain/wipers_visible");
	drs.wipers_visible.write_cb = wiper_cb;

	return (1);
errout:
	glutils_texsz_fini();
	return (0);
}

PLUGIN_API void
XPluginStop(void)
{
	if (librain_inited) {
		librain_fini();
		librain_inited = B_FALSE;
	}

	dr_delete(&drs.wipers_visible);
	dr_delete(&drs.debug_draw);
	dr_delete(&drs.verbose);
	dr_delete(&drs.librain_do_init);
	dr_delete(&drs.librain_inited);
	dr_delete(&drs.num_glass_use);

	for (int i = 0; i < MAX_GLASS; i++)
		glass_data_fini(i);
	for (int i = 0; i < MAX_Z_DEPTH_OBJS; i++)
		obj_data_fini(&z_depth_objs[i]);

	glutils_texsz_fini();

	logMsg("librain unloaded");
}

PLUGIN_API int
XPluginEnable(void)
{
	XPLMRegisterDrawCallback(draw_rain_effects, xplm_Phase_LastScene,
	    0, NULL);
	XPLMRegisterFlightLoopCallback(wiper_floop, -1, NULL);

	return (1);
}

PLUGIN_API void
XPluginDisable(void)
{
	XPLMUnregisterDrawCallback(draw_rain_effects, xplm_Phase_LastScene,
	    0, NULL);
	XPLMUnregisterFlightLoopCallback(wiper_floop, NULL);
}

PLUGIN_API void
XPluginReceiveMessage(XPLMPluginID from, int msg, void *param)
{
	UNUSED(from);
	UNUSED(msg);
	UNUSED(param);
}
