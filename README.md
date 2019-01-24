# Implementing librain to your project

## Prerequisites
 - librain latest release (https://github.com/skiselkov/librain/releases)
 - libacfutils latest release (https://github.com/skiselkov/libacfutils/releases)
 - X-Plane SDK (https://developer.x-plane.com/sdk/plugin-sdk-downloads/)
 - Understanding basic plugin structure for X-Plane and X-Plane's SDK
 - Adding a DLL (or similar on other platforms) to your project

### Adding headers
After you copied all the required inclues to their place you can include the required headers.
```cpp
#include "include/librain.h"
#include "include/obj8.h"
```

### Define window/windshield object(s)
```cpp
char *file = "path/to/your/windshield.obj";
```

### Define object's position
```cpp
vect3_t pos = { 0, 0, 0 };
```

### Parse the object 
```cpp
obj8_t *windShieldObj = obj8_parse(file, pos);
```

### Specifiy the shader's directory
```cpp
char *shaderDir = "librain-shaders";
```

### Define glass elements
Define all glass elements which should get the rain animation. You should have separate properties for each glass element, like front windshield, side window, etc. See the `librain_glass_t` below.
To get all available properties and how to use them, see https://github.com/skiselkov/librain/blob/master/src/librain.h

```cpp
float heatzones[16] = { 0 };
float temps[4] = { 0 };
float hot_air_src[4] = { 0 };
float hot_air_radius[2] = { 0 };
float hot_air_temp[2] = { 0 };
vect2_t tp = { 0.5, -0.3 };
vect2_t gp = { 0.5, 1.3 };
vect2_t wp = { 1.5, 0.5 };

static librain_glass_t windShield = {
	windShieldObj,
	NULL,               // const char    **group_ids;
	1.0,                // double        slant_factor;
	tp,                 // vect2_t       thrust_point;
	0.0,                // double        thrust_factor;
	0.0,                // double        max_thrust;
	gp,                 // vect2_t       gravity_point;
	0.5,                // double        gravity_factor;
	wp,                 // vect2_t       wind_point;
	1.0,                // double        wind_factor;
	1.0,                // double        wind_normal;
	100.0,              // double        max_tas;
	20.0,               // float         therm_inertia;
	*heatzones,         // float         heat_zones[16];
	*temps,             // float         heat_tgt_temps[4];
	22.0,               // float         cabin_temp;
	*hot_air_src,       // float         hot_air_src[4];
	*hot_air_radius,    // float         hot_air_radius[2];
	*hot_air_temp,      // float         hot_air_temp[2];
};

static librain_glass_t windShield = { ... }

static librain_glass_t glassElementsArray[1] = { windShield };
static librain_glass_t glassElementsArray[2] = { sideWindow };
...
```


### Init
```cpp

PLUGIN_API int XPluginStart(
							char *		outName,
							char *		outSig,
							char *		outDesc)
{

	librain_set_debug_draw(TRUE);
	...
	librain_init(shaderDir, glassElementsArray, 1);
	XPLMRegisterDrawCallback(draw_rain_effects, xplm_Phase_LastScene, 0, NULL);
	...
	return g_window != NULL;
}
```

Don't forget to unregister the callback when the plugin in unloaded
```cpp
PLUGIN_API void	XPluginStop(void)
{
	librain_fini();
	XPLMUnregisterDrawCallback(draw_rain_effects, xplm_Phase_LastScene, 0, NULL);
}

```

### Adding callback for drawing rain
```cpp
static int draw_rain_effects(
	XPLMDrawingPhase     inPhase,
	int                  inIsBefore,
	void *               inRefcon)
{
	librain_draw_prepare(FALSE);
	librain_draw_z_depth(windShieldObj, NULL);
	librain_draw_exec();
	librain_draw_finish();
	return 1;
}
```
You have to call `librain_draw_z_depth(...)` for all the objects which can block the view of the glass element that receives the rain animation.
You can optimize the performance by using a low-res version of these objects.

---

### Very basic sample plugin
```cpp
#include "include/librain.h"
#include "include/obj8.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMUtilities.h"
#include <string.h>

#if IBM
	#include <windows.h>
#endif
#if LIN
	#include <GL/gl.h>
#elif __GNUC__
	#include <OpenGL/gl.h>
#else
	#include <GL/gl.h>
#endif

#ifndef XPLM300
	#error This is made to be compiled against the XPLM300 SDK
#endif

char *file = "path/to/your/windshield.obj";
vect3_t pos = { 0, 0, 0 };
obj8_t *obj = obj8_parse(file, pos);

char *shaderDir = "librain-shaders";

float heatzones[16] = { 0 };
float temps[4] = { 0 };
float hot_air_src[4] = { 0 };
float hot_air_radius[2] = { 0 };
float hot_air_temp[2] = { 0 };
vect2_t tp = { 0.5, -0.3 };
vect2_t gp = { 0.5, 1.3 };
vect2_t wp = { 1.5, 0.5 };

static librain_glass_t windShield = {
	obj,
	NULL,
	1.0,
	tp, 
	0.0,
	0.0,
	gp, 
	0.5,
	wp, 
	1.0,
	1.0,
	100.0,
	20.0,
	*heatzones,
	*temps,
	22.0,
	*hot_air_src,
	*hot_air_radius,
	*hot_air_temp,
};

static librain_glass_t glassElementsArray[1] = { windShield };

static int draw_rain_effects(
	XPLMDrawingPhase     inPhase,
	int                  inIsBefore,
	void *               inRefcon)
{
	librain_draw_prepare(FALSE);
	librain_draw_z_depth(compassObj, NULL);
	librain_draw_z_depth(fuselageObj, NULL);
	librain_draw_z_depth(windShieldObj, NULL);
	librain_draw_exec();
	librain_draw_finish();
	return 1;
}

int                 dummy_mouse_handler(XPLMWindowID in_window_id, int x, int y, int is_down, void * in_refcon) { return 0; }
XPLMCursorStatus    dummy_cursor_status_handler(XPLMWindowID in_window_id, int x, int y, void * in_refcon) { return xplm_CursorDefault; }
int                 dummy_wheel_handler(XPLMWindowID in_window_id, int x, int y, int wheel, int clicks, void * in_refcon) { return 0; }
void                dummy_key_handler(XPLMWindowID in_window_id, char key, XPLMKeyFlags flags, char virtual_key, void * in_refcon, int losing_focus) { }


PLUGIN_API int XPluginStart(
							char *		outName,
							char *		outSig,
							char *		outDesc)
{

	librain_set_debug_draw(TRUE);
	strcpy(outName, "HelloWorld3RainPlugin");
	strcpy(outSig, "librain.hello.world");
	strcpy(outDesc, "A Hello World plug-in for librain");
	
	librain_init(shaderDir, glassElementsArray, 1);

	XPLMRegisterDrawCallback(draw_rain_effects, xplm_Phase_LastScene, 0, NULL);
	
	return g_window != NULL;
}

PLUGIN_API void	XPluginStop(void)
{
	librain_fini();
	XPLMUnregisterDrawCallback(draw_rain_effects, xplm_Phase_LastScene, 0, NULL);
}

PLUGIN_API void XPluginDisable(void) { }
PLUGIN_API int  XPluginEnable(void)  { return 1; }
PLUGIN_API void XPluginReceiveMessage(XPLMPluginID inFrom, int inMsg, void * inParam) { }
```
