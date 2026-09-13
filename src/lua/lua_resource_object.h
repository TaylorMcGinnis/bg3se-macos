/**
 * BG3SE-macOS - Lua proxies for GuidResource objects
 *
 * Ext.StaticData.Get/Create hand back a live pointer into the game's resource
 * banks. These proxies put a field interface on it, driven by the generated
 * layout table, so mods can read and write resource fields the way they do on
 * Windows instead of getting an opaque userdata.
 *
 * Nothing is copied: the proxy holds the resource pointer and reads through it
 * on every access, so a write is immediately visible to the game.
 */

#ifndef BG3SE_LUA_RESOURCE_OBJECT_H
#define BG3SE_LUA_RESOURCE_OBJECT_H

#include <lua.h>
#include <stdbool.h>

/**
 * Push a resource as a field proxy. Falls back to a light userdata if the type
 * has no generated layout, so callers always get something truthy back.
 */
void lua_resource_object_push(lua_State *L, void *obj, const char *type_name);

struct ResourceLayout;

/**
 * Push a field proxy over an object using an explicit layout, for callers
 * (root templates) whose layouts are not in the GuidResource table. The layout
 * must outlive the proxy; use `embedded = true` so the GuidResource-only keys
 * (ResourceUUID) are not offered.
 */
void lua_resource_object_push_layout(lua_State *L, void *obj, const struct ResourceLayout *layout);

/**
 * Ext.Types.Serialize / Unserialize for resource-object proxies (root
 * templates and static-data objects).
 *
 * Scalar fields only; arrays and nested structs are skipped, as writing them
 * back needs ownership operations this layer cannot perform. Both go through
 * the proxy's own __index / __newindex, so the resource layer's conversions and
 * read-only rules apply. Unserialize may raise out of __newindex for a refused
 * field, as the component path does.
 *
 * Return false when the value is not a resource-object proxy, so callers can
 * fall through to other proxy kinds.
 */
bool lua_resource_object_serialize(lua_State *L, int index);
bool lua_resource_object_unserialize(lua_State *L, int objIndex, int tableIndex);

#endif // BG3SE_LUA_RESOURCE_OBJECT_H
