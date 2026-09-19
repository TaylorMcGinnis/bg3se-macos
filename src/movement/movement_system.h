#ifndef BG3SE_MOVEMENT_SYSTEM_H
#define BG3SE_MOVEMENT_SYSTEM_H

#include <lua.h>
#include <stdbool.h>

/**
 * Suspend or resume a requested keyboard-movement unlock as the active camera
 * enters or leaves combat. Safe to call repeatedly from the camera update.
 */
void movement_update_combat_state(bool combat);

/** Register the build-gated Ext.Movement namespace. */
void lua_movement_register(lua_State *L, int ext_table_idx);

#endif
