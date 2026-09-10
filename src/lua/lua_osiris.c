/**
 * BG3SE-macOS - Lua Osiris Namespace Implementation
 *
 * Handles registration of Osiris event listeners and custom functions.
 */

#include "lua_osiris.h"
#include "lua_context.h"
#include "custom_functions.h"
#include "logging.h"

#include <stdlib.h>
#include <string.h>

// ============================================================================
// Context Guard Helpers
// ============================================================================

/**
 * Check if current context allows Osiris operations.
 * Osiris is server-side, so operations should be in SERVER context.
 * Returns 1 if OK to proceed, 0 if context mismatch (logs warning).
 */
static int check_osiris_context(const char *operation) {
    LuaContext ctx = lua_context_get();
    if (ctx == LUA_CONTEXT_SERVER) {
        return 1;  // OK - server context
    }
    if (ctx == LUA_CONTEXT_NONE) {
        // During initialization, allow it but log
        LOG_LUA_DEBUG("Osiris.%s called during initialization (context=None)", operation);
        return 1;
    }
    // Client context - warn but allow for compatibility
    LOG_LUA_DEBUG("Warning: Osiris.%s called in Client context (should be Server)", operation);
    return 1;  // Allow for now, but logged
}

// ============================================================================
// Internal State
// ============================================================================

// Growable: upstream's OsirisCallbackManager has no subscription cap, and a
// large mod profile registers well over the old fixed 512 (Wave 5 exhausted
// 64; the 744-mod profile in bg3se-nominal-limits broke every fixed cap).
/* Set by main.c: binds a late-registered listener to its RETE node, the way
 * upstream's Subscribe() calls RegisterNodeHandler when the story is already
 * loaded. Without it, a listener created after story load never fires. */
static OsirisNodeBindFn g_node_binder = NULL;

void lua_osiris_set_node_binder(OsirisNodeBindFn fn) { g_node_binder = fn; }

static OsirisListener *osiris_listeners = NULL;
static int osiris_listener_count = 0;
static int osiris_listener_capacity = 0;

static OsirisListener *osiris_listener_alloc(void) {
    if (osiris_listener_count >= osiris_listener_capacity) {
        int cap = osiris_listener_capacity ? osiris_listener_capacity * 2
                                           : MAX_OSIRIS_LISTENERS;
        OsirisListener *grown = realloc(osiris_listeners,
                                        (size_t)cap * sizeof(OsirisListener));
        if (!grown) return NULL;
        osiris_listeners = grown;
        osiris_listener_capacity = cap;
    }
    return &osiris_listeners[osiris_listener_count];
}

// ============================================================================
// Lua C API Functions
// ============================================================================

int lua_ext_osiris_registerlistener(lua_State *L) {
    check_osiris_context("RegisterListener");

    const char *event = luaL_checkstring(L, 1);
    int arity = (int)luaL_checkinteger(L, 2);
    const char *timing = luaL_checkstring(L, 3);
    luaL_checktype(L, 4, LUA_TFUNCTION);

    // Upstream RegisterOsirisListener (Lua/Server/LuaServer.cpp) accepts
    // exactly these four hook types and raises for anything else; a typo
    // here used to register a listener that could never fire.
    if (strcmp(timing, "before") != 0 && strcmp(timing, "after") != 0 &&
        strcmp(timing, "beforeDelete") != 0 && strcmp(timing, "afterDelete") != 0) {
        return luaL_error(L, "Hook type must be 'before', 'beforeDelete', 'after' or 'afterDelete'");
    }

    OsirisListener *listener = osiris_listener_alloc();
    if (!listener) {
        return luaL_error(L, "RegisterListener: out of memory growing the listener table (%d)",
                          osiris_listener_count);
    }

    // Store the listener
    strncpy(listener->event_name, event, sizeof(listener->event_name) - 1);
    listener->event_name[sizeof(listener->event_name) - 1] = '\0';
    listener->arity = arity;
    strncpy(listener->timing, timing, sizeof(listener->timing) - 1);
    listener->timing[sizeof(listener->timing) - 1] = '\0';

    // Store callback reference in Lua registry
    lua_pushvalue(L, 4);  // Push the function
    listener->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    osiris_listener_count++;

    LOG_LUA_DEBUG("Registered Osiris listener: %s (arity=%d, timing=%s)",
                event, arity, timing);

    /* Bind to the story node now if the story is already loaded (upstream
     * Subscribe -> RegisterNodeHandler). Engine events are ignored by the
     * binder and keep using the Event hook. */
    if (g_node_binder) g_node_binder(event, arity);

    // Return the 1-based listener index as the subscription id (what
    // UnregisterListener takes), like upstream's SubscriptionId.
    lua_pushinteger(L, osiris_listener_count);
    return 1;
}

int lua_ext_osiris_unregisterlistener(lua_State *L) {
    lua_Integer id = luaL_checkinteger(L, 1);

    // Tombstone rather than compact: ids are table indices, and a dispatch
    // loop may be walking the table when a handler unregisters itself.
    if (id < 1 || id > osiris_listener_count ||
        osiris_listeners[id - 1].callback_ref == LUA_NOREF) {
        lua_pushboolean(L, 0);
        return 1;
    }

    OsirisListener *listener = &osiris_listeners[id - 1];
    luaL_unref(L, LUA_REGISTRYINDEX, listener->callback_ref);
    listener->callback_ref = LUA_NOREF;

    LOG_LUA_DEBUG("Unregistered Osiris listener #%lld: %s (arity=%d, timing=%s)",
                  (long long)id, listener->event_name, listener->arity, listener->timing);

    lua_pushboolean(L, 1);
    return 1;
}

// ============================================================================
// Custom Function Registration
// ============================================================================

int lua_ext_osiris_newcall(lua_State *L) {
    check_osiris_context("NewCall");

    const char *name = luaL_checkstring(L, 1);
    const char *signature = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    // Store callback in registry
    lua_pushvalue(L, 3);
    int callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // Register the custom call
    uint32_t handle = custom_func_register(name, CUSTOM_FUNC_CALL, callback_ref, signature);
    if (handle == 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
        return luaL_error(L, "Failed to register custom call '%s'", name);
    }

    LOG_LUA_DEBUG("Ext.Osiris.NewCall: registered '%s' (ID=0x%x)", name, handle);

    lua_pushinteger(L, handle);
    return 1;
}

int lua_ext_osiris_newquery(lua_State *L) {
    check_osiris_context("NewQuery");

    const char *name = luaL_checkstring(L, 1);
    const char *signature = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    // Store callback in registry
    lua_pushvalue(L, 3);
    int callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // Register the custom query
    uint32_t handle = custom_func_register(name, CUSTOM_FUNC_QUERY, callback_ref, signature);
    if (handle == 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
        return luaL_error(L, "Failed to register custom query '%s'", name);
    }

    LOG_LUA_DEBUG("Ext.Osiris.NewQuery: registered '%s' (ID=0x%x)", name, handle);

    lua_pushinteger(L, handle);
    return 1;
}

int lua_ext_osiris_newevent(lua_State *L) {
    check_osiris_context("NewEvent");

    const char *name = luaL_checkstring(L, 1);
    const char *signature = luaL_checkstring(L, 2);

    // Events don't have callbacks - they're raised from Lua
    uint32_t handle = custom_func_register(name, CUSTOM_FUNC_EVENT, LUA_NOREF, signature);
    if (handle == 0) {
        return luaL_error(L, "Failed to register custom event '%s'", name);
    }

    LOG_LUA_DEBUG("Ext.Osiris.NewEvent: registered '%s' (ID=0x%x)", name, handle);

    lua_pushinteger(L, handle);
    return 1;
}

/**
 * Ext.Osiris.RaiseEvent(name, ...)
 *
 * Raises a custom Osiris event, dispatching to all registered listeners.
 *
 * Example:
 *   Ext.Osiris.NewEvent("MyMod_ItemCollected", "(GUIDSTRING)_Item,(GUIDSTRING)_Collector")
 *   -- Later:
 *   Ext.Osiris.RaiseEvent("MyMod_ItemCollected", itemGuid, playerGuid)
 *   -- Listeners registered via RegisterListener will receive the event
 */
int lua_ext_osiris_raiseevent(lua_State *L) {
    check_osiris_context("RaiseEvent");

    const char *eventName = luaL_checkstring(L, 1);

    // Find the custom event
    CustomFunction *func = custom_func_get_by_name(eventName);
    if (!func) {
        return luaL_error(L, "RaiseEvent: unknown event '%s'", eventName);
    }
    if (func->type != CUSTOM_FUNC_EVENT) {
        return luaL_error(L, "RaiseEvent: '%s' is not an event (it's a %s)",
                         eventName,
                         func->type == CUSTOM_FUNC_CALL ? "Call" : "Query");
    }

    // Get the number of arguments passed (excluding event name)
    int nargs = lua_gettop(L) - 1;
    if ((uint32_t)nargs != func->arity) {
        return luaL_error(L, "RaiseEvent: '%s' expects %d arguments, got %d",
                         eventName, func->arity, nargs);
    }

    LOG_LUA_DEBUG("RaiseEvent: raising '%s' with %d arguments", eventName, nargs);

    // Dispatch to all registered listeners
    int listener_count = lua_osiris_get_listener_count();
    int dispatched = 0;

    for (int i = 0; i < listener_count; i++) {
        OsirisListener *listener = lua_osiris_get_listener(i);
        if (!listener) continue;

        // Match by event name (timing doesn't matter for custom events - call both "before" and "after")
        if (strcmp(listener->event_name, eventName) != 0) continue;

        // The handler may register a listener (reallocating the table), so
        // take what we need before calling into Lua.
        int callback_ref = listener->callback_ref;
        int argsToPass = (listener->arity < nargs) ? listener->arity : nargs;

        // Get callback from Lua registry
        lua_pushcfunction(L, lua_osiris_traceback_msgh);
        int msgh = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, callback_ref);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 2);
            continue;
        }

        // Push arguments (up to listener's requested arity)
        for (int j = 0; j < argsToPass; j++) {
            lua_pushvalue(L, j + 2);  // +2 because arg 1 is eventName
        }

        // Call the callback
        if (lua_pcall(L, argsToPass, 0, msgh) != LUA_OK) {
            LOG_OSIRIS_ERROR("Osiris event handler failed: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else {
            dispatched++;
        }
        lua_remove(L, msgh);
    }

    LOG_LUA_DEBUG("RaiseEvent: '%s' dispatched to %d listeners", eventName, dispatched);

    // Return number of listeners called
    lua_pushinteger(L, dispatched);
    return 1;
}

/**
 * Ext.Osiris.GetCustomFunctions() -> table
 *
 * Returns a table of all registered custom functions:
 * {
 *   ["FunctionName"] = {
 *     Type = "Call" | "Query" | "Event",
 *     Arity = number,
 *     InParams = number,
 *     OutParams = number,
 *     Id = number
 *   },
 *   ...
 * }
 */
int lua_ext_osiris_getcustomfunctions(lua_State *L) {
    lua_newtable(L);  // Result table

    int count = custom_func_get_count();
    for (int i = 0; i < count; i++) {
        CustomFunction *func = custom_func_get_by_index(i);
        if (!func) continue;

        // Create entry table
        lua_newtable(L);

        // Type field
        const char *type_str = (func->type == CUSTOM_FUNC_CALL) ? "Call" :
                               (func->type == CUSTOM_FUNC_QUERY) ? "Query" : "Event";
        lua_pushstring(L, type_str);
        lua_setfield(L, -2, "Type");

        // Arity field
        lua_pushinteger(L, (lua_Integer)func->arity);
        lua_setfield(L, -2, "Arity");

        // InParams field
        lua_pushinteger(L, (lua_Integer)func->num_in_params);
        lua_setfield(L, -2, "InParams");

        // OutParams field
        lua_pushinteger(L, (lua_Integer)func->num_out_params);
        lua_setfield(L, -2, "OutParams");

        // Id field (as hex-friendly integer)
        lua_pushinteger(L, (lua_Integer)func->assigned_id);
        lua_setfield(L, -2, "Id");

        // Set this table as result[func->name]
        lua_setfield(L, -2, func->name);
    }

    return 1;
}

// ============================================================================
// Listener Access Functions
// ============================================================================

int lua_osiris_get_listener_count(void) {
    return osiris_listener_count;
}

OsirisListener *lua_osiris_get_listener(int index) {
    if (index < 0 || index >= osiris_listener_count) {
        return NULL;
    }
    if (osiris_listeners[index].callback_ref == LUA_NOREF) {
        return NULL;   /* unregistered */
    }
    return &osiris_listeners[index];
}

int lua_osiris_traceback_msgh(lua_State *L) {
    // Upstream CallWithTraceback: append the Lua traceback to the message so
    // "Osiris event handler failed:" names the mod line, not just the error.
    const char *msg = lua_tostring(L, 1);
    if (msg) {
        luaL_traceback(L, L, msg, 1);
    } else if (!lua_isnoneornil(L, 1)) {
        if (!luaL_callmeta(L, 1, "__tostring")) {
            lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
        }
    } else {
        lua_pushstring(L, "(nil error)");
    }
    return 1;
}

void lua_osiris_reset_listeners(lua_State *L) {
    // Currently uncalled (the server VM is not recreated per session yet).
    // Releases every registry reference so a reset doesn't leak them.
    if (L) {
        for (int i = 0; i < osiris_listener_count; i++) {
            if (osiris_listeners[i].callback_ref != LUA_NOREF) {
                luaL_unref(L, LUA_REGISTRYINDEX, osiris_listeners[i].callback_ref);
                osiris_listeners[i].callback_ref = LUA_NOREF;
            }
        }
    }
    osiris_listener_count = 0;
}

void lua_osiris_reset_custom_functions(lua_State *L) {
    custom_func_clear(L);
}

// ============================================================================
// Registration
// ============================================================================

void lua_osiris_register(lua_State *L) {
    // Initialize custom function registry
    custom_func_init();

    // Get Ext table
    lua_getglobal(L, "Ext");

    // Create Ext.Osiris table
    lua_newtable(L);

    lua_pushcfunction(L, lua_ext_osiris_registerlistener);
    lua_setfield(L, -2, "RegisterListener");

    lua_pushcfunction(L, lua_ext_osiris_unregisterlistener);
    lua_setfield(L, -2, "UnregisterListener");

    lua_pushcfunction(L, lua_ext_osiris_newcall);
    lua_setfield(L, -2, "NewCall");

    lua_pushcfunction(L, lua_ext_osiris_newquery);
    lua_setfield(L, -2, "NewQuery");

    lua_pushcfunction(L, lua_ext_osiris_newevent);
    lua_setfield(L, -2, "NewEvent");

    lua_pushcfunction(L, lua_ext_osiris_getcustomfunctions);
    lua_setfield(L, -2, "GetCustomFunctions");

    lua_pushcfunction(L, lua_ext_osiris_raiseevent);
    lua_setfield(L, -2, "RaiseEvent");

    lua_setfield(L, -2, "Osiris");

    lua_pop(L, 1);  // Pop Ext table

    LOG_OSIRIS_INFO("Ext.Osiris API registered (with NewCall/NewQuery/NewEvent)");
}
