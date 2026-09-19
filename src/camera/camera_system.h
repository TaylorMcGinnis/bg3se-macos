#ifndef BG3SE_CAMERA_SYSTEM_H
#define BG3SE_CAMERA_SYSTEM_H

#include <lua.h>
#include <stdbool.h>

/** Feed raw macOS mouse deltas into the native camera controller. */
void camera_input_mouse_delta(double delta_x, double delta_y);

/** Select the configured close/far camera distance from a wheel delta. */
void camera_input_scroll(double delta_y);

/** Enable hiding BG3's UI while Caps Lock mouse-look is active. */
bool camera_set_hide_ui_with_mouse_look(bool enabled, const char **reason);

/** Read BG3's current presentation-mode state. */
bool camera_get_game_ui_hidden(bool *hidden, const char **reason);

/** Register the native Ext.Camera namespace on an Ext table. */
void lua_camera_register(lua_State *L, int ext_table_idx);

#endif
