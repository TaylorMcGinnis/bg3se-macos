/**
 * BG3SE-macOS - Lua Osiris Namespace Module
 *
 * Provides Ext.Osiris API for registering callbacks for Osiris events.
 */

#ifndef BG3SE_LUA_OSIRIS_H
#define BG3SE_LUA_OSIRIS_H

#include <lua.h>
#include <lauxlib.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Constants
// ============================================================================

// Initial listener-table capacity. The table grows on demand (upstream's
// OsirisCallbackManager has no cap); this is only the first allocation.
#define MAX_OSIRIS_LISTENERS 512

// ============================================================================
// Data Structures
// ============================================================================

/**
 * Registered Osiris event listener
 */
typedef struct {
    char event_name[128];    // Osiris event name to listen for
    int arity;               // Number of arguments the callback expects
    char timing[16];         // "before", "after", "beforeDelete" or "afterDelete"
    int callback_ref;        // Lua registry reference; LUA_NOREF once unregistered
} OsirisListener;

// ============================================================================
// Lua C API Functions
// ============================================================================

/**
 * Ext.Osiris.RegisterListener(event, arity, timing, callback) -> id
 * Registers a callback for an Osiris event. Returns the subscription id.
 */
int lua_ext_osiris_registerlistener(lua_State *L);

/**
 * Ext.Osiris.UnregisterListener(id) -> boolean
 * Removes a subscription made by RegisterListener; false if the id is not a
 * live subscription (upstream OsirisCallbackManager::Unsubscribe).
 */
int lua_ext_osiris_unregisterlistener(lua_State *L);

/**
 * Ext.Osiris.NewCall(name, signature, handler)
 * Registers a custom Osiris call (no return value).
 */
int lua_ext_osiris_newcall(lua_State *L);

/**
 * Ext.Osiris.NewQuery(name, signature, handler)
 * Registers a custom Osiris query (returns values via OUT params).
 */
int lua_ext_osiris_newquery(lua_State *L);

/**
 * Ext.Osiris.NewEvent(name, signature)
 * Registers a custom Osiris event (can be raised from Lua).
 */
int lua_ext_osiris_newevent(lua_State *L);

/**
 * Ext.Osiris.GetCustomFunctions()
 * Returns a table of all registered custom functions (for debugging).
 */
int lua_ext_osiris_getcustomfunctions(lua_State *L);

/**
 * Ext.Osiris.RaiseEvent(name, ...)
 * Raises a custom Osiris event, dispatching to registered listeners.
 */
int lua_ext_osiris_raiseevent(lua_State *L);

// ============================================================================
// Listener Access Functions
// ============================================================================

/**
 * Get the total number of registered listeners.
 */
int lua_osiris_get_listener_count(void);

/**
 * Get a listener by index.
 * @return Pointer to listener, or NULL if index out of range or the
 *         subscription was unregistered. The pointer is only valid until the
 *         next RegisterListener (the table may be reallocated), so copy what
 *         you need before calling back into Lua.
 */
OsirisListener *lua_osiris_get_listener(int index);

/**
 * lua_pcall message handler that appends a Lua traceback to the error
 * (upstream CallWithTraceback). Push with lua_pushcfunction and pass its
 * stack index as the msgh argument.
 */
int lua_osiris_traceback_msgh(lua_State *L);

/**
 * Late node binding. Upstream's OsirisCallbackManager::Subscribe registers the
 * node handler immediately when the story is already loaded, so a listener
 * added after StoryLoaded still fires. main.c installs this; RegisterListener
 * calls it. NULL until the Osiris side is up.
 */
typedef void (*OsirisNodeBindFn)(const char *name, int arity);
void lua_osiris_set_node_binder(OsirisNodeBindFn fn);

/**
 * Reset all listeners (for cleanup), releasing their registry references.
 * @param L Lua state that owns the callback references (may be NULL if the
 *          state is already closed)
 */
void lua_osiris_reset_listeners(lua_State *L);

/**
 * Reset all custom functions (for session cleanup).
 * @param L Lua state (to release callback references)
 */
void lua_osiris_reset_custom_functions(lua_State *L);

// ============================================================================
// Registration
// ============================================================================

/**
 * Register Ext.Osiris namespace functions.
 * @param L Lua state with Ext table on top of stack
 */
void lua_osiris_register(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif // BG3SE_LUA_OSIRIS_H
