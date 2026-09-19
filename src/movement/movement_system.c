/**
 * Keyboard movement unlock for native macOS BG3.
 *
 * BG3 already owns the complete direct-movement path. Keyboard bindings for
 * CharacterMoveForward/Backward/Left/Right reach that path, but
 * CharacterTask_MoveController::CanExecute() rejects it while the UI is in
 * keyboard/mouse mode. This module neutralizes that guard and changes the
 * camera input handler's should_move store from 1 to 0, preventing the same
 * W/A/S/D input from panning the map camera. It does not set controller mode,
 * synthesize controller input, or alter the UI.
 */

#include "movement_system.h"

/* Per-store patch sites; -DBG3_STORE selects src/gen/<store>/. */
#include "camera_addresses.h"

#include "../core/safe_memory.h"
#include "../core/version_detect.h"
#include "../core/offset_table.h"
#include "../core/logging.h"
#include "../hooks/arm64_hook.h"
#include "../core/safe_memory.h"

#include <dobby.h>

#include <lauxlib.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define MOVEMENT_UNLOCK_ORIGINAL_7398727      0x340016e8U
#define CAMERA_SHOULD_MOVE_ORIGINAL_7398727     0x390512dbU
#define CAMERA_SHOULD_MOVE_BLOCKED_7398727      0x390512dfU
#define ARM64_NOP                             0xd503201fU

/* CanExecute loads EoCGlobalSwitches then tests the dword at this offset; when
 * it is zero the whole GetRotatedInput path is skipped, so direct movement can
 * never engage regardless of the CanExecute patch. */
#define GLOBAL_SWITCH_DIRECT_MOVE_OFFSET      0xe98

/* Requested remains true while combat temporarily restores vanilla behavior. */
static _Atomic(bool) g_movement_unlock_requested;
static _Atomic(bool) g_combat_active;

/* Saved so disabling restores whatever the game had, rather than forcing 0. */
static _Atomic(int) g_direct_move_saved = -1;

static uint32_t *direct_move_switch_address(void) {
    const VersionOffsets *off = offset_table_get();
    if (!off || !off->global_switches_ptr) return NULL;
    void **slot = (void **)offset_table_resolve(off->global_switches_ptr);
    void *switches = NULL;
    if (!slot || !safe_memory_read_pointer((mach_vm_address_t)slot, &switches) ||
        !switches) {
        return NULL;
    }
    return (uint32_t *)((char *)switches + GLOBAL_SWITCH_DIRECT_MOVE_OFFSET);
}

/* --- read-only probe: is the selector-mode guard what rejects CanExecute? -- */
static _Atomic(int) g_selector_last = -1;      /* -1 = never observed */
static _Atomic(uint64_t) g_selector_calls;
static bool (*g_orig_selector_mode)(void *, void *);
static bool g_selector_hooked;

static bool selector_mode_probe(void *self, void *view) {
    bool r = g_orig_selector_mode(self, view);
    atomic_store_explicit(&g_selector_last, r ? 1 : 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_selector_calls, 1, memory_order_relaxed);
    return r;
}

static const uint8_t kSelectorPrologue7398727[] = {
    0xff, 0x43, 0x07, 0xd1, 0xfc, 0x6f, 0x17, 0xa9,
    0xfa, 0x67, 0x18, 0xa9, 0xf8, 0x5f, 0x19, 0xa9
};

static void selector_probe_install(void) {
    if (g_selector_hooked || INPUT_IS_IN_SELECTOR_MODE_OFFSET_7398727 == 0) return;
    if (!version_detect_addresses_safe()) return;
    void *base = version_detect_get_binary_base();
    if (!base) return;
    void *target = (char *)base + INPUT_IS_IN_SELECTOR_MODE_OFFSET_7398727;
    uint8_t seen[sizeof(kSelectorPrologue7398727)];
    if (!safe_memory_read((mach_vm_address_t)target, seen, sizeof(seen)) ||
        memcmp(seen, kSelectorPrologue7398727, sizeof(seen)) != 0) {
        LOG_CORE_ERROR("[Movement] selector probe: signature mismatch");
        g_selector_hooked = true;   /* fail closed, do not retry */
        return;
    }
    if (DobbyHook(target, (void *)selector_mode_probe,
                  (void **)&g_orig_selector_mode) != 0 || !g_orig_selector_mode) {
        LOG_CORE_ERROR("[Movement] selector probe: hook failed");
        g_selector_hooked = true;
        return;
    }
    g_selector_hooked = true;
    LOG_CORE_INFO("[Movement] selector-mode probe installed at %p", target);
}

static bool movement_build_supported(void) {
#if defined(__APPLE__) && defined(__arm64__)
    const char *version = version_detect_get_version();
    return version && strcmp(version, "4.1.1.7398727") == 0 &&
        version_detect_addresses_safe() &&
        version_detect_get_binary_base() != NULL;
#else
    return false;
#endif
}

static void *movement_unlock_address(void) {
    void *base = version_detect_get_binary_base();
    return base ? (char *)base + MOVEMENT_UNLOCK_BRANCH_OFFSET_7398727 : NULL;
}

static void *camera_should_move_store_address(void) {
    void *base = version_detect_get_binary_base();
    return base ? (char *)base + CAMERA_SHOULD_MOVE_STORE_OFFSET_7398727 : NULL;
}

static bool movement_read_instruction(uint32_t *out) {
    void *address = movement_unlock_address();
    return address && out && safe_memory_read(
        (mach_vm_address_t)address, out, sizeof(*out));
}

static bool camera_read_instruction(uint32_t *out) {
    void *address = camera_should_move_store_address();
    return address && out && safe_memory_read(
        (mach_vm_address_t)address, out, sizeof(*out));
}

static bool movement_set_patch_state(bool enabled) {
    uint32_t movement_instruction = 0;
    uint32_t camera_instruction = 0;
    if (!movement_build_supported() ||
        !movement_read_instruction(&movement_instruction) ||
        !camera_read_instruction(&camera_instruction)) {
        return false;
    }
    if ((movement_instruction != MOVEMENT_UNLOCK_ORIGINAL_7398727 &&
         movement_instruction != ARM64_NOP) ||
        (camera_instruction != CAMERA_SHOULD_MOVE_ORIGINAL_7398727 &&
         camera_instruction != CAMERA_SHOULD_MOVE_BLOCKED_7398727)) {
        return false;
    }

    if (enabled) {
        /*
         * CanExecute loads EoCGlobalSwitches and branches past GetRotatedInput
         * when the dword at +0xe98 is zero, so the CanExecute patch alone can
         * never produce movement: the call is skipped, not rejected. Measured
         * zero on this install. Setting the engine's own switch is preferred
         * over patching the branch -- it is the value the engine already reads.
         */
        uint32_t *sw = direct_move_switch_address();
        if (sw) {
            uint32_t cur = 0;
            if (safe_memory_read((mach_vm_address_t)sw, &cur, sizeof(cur))) {
                if (atomic_load_explicit(&g_direct_move_saved,
                                         memory_order_acquire) < 0) {
                    atomic_store_explicit(&g_direct_move_saved, (int)cur,
                                          memory_order_release);
                }
                if (cur == 0) {
                    uint32_t on = 1;
                    if (safe_memory_write((mach_vm_address_t)sw, &on, sizeof(on))) {
                        LOG_CORE_INFO("[Movement] direct-move switch was 0; set to 1");
                    } else {
                        LOG_CORE_ERROR("[Movement] could not set direct-move switch");
                    }
                }
            }
        }

        bool installed_camera_patch = false;
        if (camera_instruction == CAMERA_SHOULD_MOVE_ORIGINAL_7398727) {
            if (!arm64_write_instruction(camera_should_move_store_address(),
                                         CAMERA_SHOULD_MOVE_BLOCKED_7398727)) {
                return false;
            }
            installed_camera_patch = true;
        }
        if (movement_instruction == MOVEMENT_UNLOCK_ORIGINAL_7398727 &&
            !arm64_write_instruction(movement_unlock_address(), ARM64_NOP)) {
            if (installed_camera_patch) {
                arm64_write_instruction(camera_should_move_store_address(),
                                        CAMERA_SHOULD_MOVE_ORIGINAL_7398727);
            }
            return false;
        }
    } else {
        int saved = atomic_exchange_explicit(&g_direct_move_saved, -1,
                                             memory_order_acq_rel);
        uint32_t *sw = direct_move_switch_address();
        if (sw && saved >= 0) {
            uint32_t restore = (uint32_t)saved;
            safe_memory_write((mach_vm_address_t)sw, &restore, sizeof(restore));
        }

        if (movement_instruction == ARM64_NOP &&
            !arm64_write_instruction(movement_unlock_address(),
                                     MOVEMENT_UNLOCK_ORIGINAL_7398727)) {
            return false;
        }
        if (camera_instruction == CAMERA_SHOULD_MOVE_BLOCKED_7398727 &&
            !arm64_write_instruction(camera_should_move_store_address(),
                                     CAMERA_SHOULD_MOVE_ORIGINAL_7398727)) {
            return false;
        }
    }
    return true;
}

void movement_update_combat_state(bool combat) {
    bool previous = atomic_exchange_explicit(&g_combat_active, combat,
                                             memory_order_acq_rel);
    if (previous == combat ||
        !atomic_load_explicit(&g_movement_unlock_requested,
                              memory_order_acquire)) {
        return;
    }

    if (movement_set_patch_state(!combat)) {
        LOG_CORE_INFO("[Movement] %s combat: %s keyboard movement and camera pan",
                      combat ? "Entered" : "Left",
                      combat ? "restored vanilla" : "resumed direct");
    } else {
        LOG_CORE_ERROR("[Movement] Could not %s keyboard movement at combat transition",
                       combat ? "suspend" : "resume");
    }
}

/* Writes BG3's own setting rather than patching code. */
static int lua_movement_set_direct_move_switch(lua_State *L) {
    uint32_t want = (uint32_t)luaL_checkinteger(L, 1);
    uint32_t *sw = direct_move_switch_address();
    if (!sw) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "EoCGlobalSwitches not resolvable");
        return 2;
    }
    if (!safe_memory_write((mach_vm_address_t)sw, &want, sizeof(want))) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "write failed");
        return 2;
    }
    LOG_CORE_INFO("[Movement] direct-move switch set to %u", want);
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_movement_get_capabilities(lua_State *L) {
    uint32_t movement_instruction = 0;
    uint32_t camera_instruction = 0;
    bool supported = movement_build_supported();
    bool movement_signature_ok = supported &&
        movement_read_instruction(&movement_instruction) &&
        (movement_instruction == MOVEMENT_UNLOCK_ORIGINAL_7398727 ||
         movement_instruction == ARM64_NOP);
    bool camera_signature_ok = supported &&
        camera_read_instruction(&camera_instruction) &&
        (camera_instruction == CAMERA_SHOULD_MOVE_ORIGINAL_7398727 ||
         camera_instruction == CAMERA_SHOULD_MOVE_BLOCKED_7398727);
    bool signature_ok = movement_signature_ok && camera_signature_ok;

    lua_newtable(L);
    lua_pushboolean(L, signature_ok);
    lua_setfield(L, -2, "KeyboardMovement");
    bool requested = atomic_load_explicit(&g_movement_unlock_requested,
                                          memory_order_acquire);
    bool combat = atomic_load_explicit(&g_combat_active,
                                       memory_order_acquire);
    lua_pushboolean(L, requested && !combat &&
        movement_instruction == ARM64_NOP &&
        camera_instruction == CAMERA_SHOULD_MOVE_BLOCKED_7398727);
    lua_setfield(L, -2, "Enabled");
    lua_pushboolean(L, requested);
    lua_setfield(L, -2, "Requested");
    lua_pushboolean(L, requested && combat);
    lua_setfield(L, -2, "SuspendedForCombat");
    lua_pushboolean(L, camera_signature_ok);
    lua_setfield(L, -2, "CameraPanBlock");

    {
        uint32_t *sw = direct_move_switch_address();
        uint32_t v = 0;
        if (sw && safe_memory_read((mach_vm_address_t)sw, &v, sizeof(v))) {
            lua_pushinteger(L, (lua_Integer)v);
        } else {
            lua_pushnil(L);
        }
        lua_setfield(L, -2, "DirectMoveSwitch");
    }

    selector_probe_install();
    lua_pushinteger(L, (lua_Integer)atomic_load_explicit(&g_selector_last,
                                                         memory_order_relaxed));
    lua_setfield(L, -2, "SelectorModeLast");
    lua_pushnumber(L, (lua_Number)atomic_load_explicit(&g_selector_calls,
                                                       memory_order_relaxed));
    lua_setfield(L, -2, "SelectorModeCalls");

    /* Live value of the task selector, so a failure to move can be told apart
     * from a failure to patch. */
    if (CORE_INPUT_MODE_FLAG_OFFSET_7398727 != 0 && supported) {
        void *base = version_detect_get_binary_base();
        uint8_t mode = 0;
        if (base && safe_memory_read(
                (mach_vm_address_t)((char *)base +
                    CORE_INPUT_MODE_FLAG_OFFSET_7398727), &mode, sizeof(mode))) {
            lua_pushinteger(L, (lua_Integer)mode);
        } else {
            lua_pushnil(L);
        }
        lua_setfield(L, -2, "InputModeFlag");
    }
    lua_pushboolean(L,
        camera_instruction == CAMERA_SHOULD_MOVE_BLOCKED_7398727);
    lua_setfield(L, -2, "CameraPanBlocked");
    lua_pushinteger(L, 157); lua_setfield(L, -2, "ForwardCommand");
    lua_pushinteger(L, 158); lua_setfield(L, -2, "BackwardCommand");
    lua_pushinteger(L, 159); lua_setfield(L, -2, "LeftCommand");
    lua_pushinteger(L, 160); lua_setfield(L, -2, "RightCommand");
    lua_pushstring(L, version_detect_get_version() ?
        version_detect_get_version() : "unknown");
    lua_setfield(L, -2, "Build");
    if (!supported) {
        lua_pushstring(L, "keyboard movement is unavailable on this build or architecture");
        lua_setfield(L, -2, "Reason");
    } else if (!signature_ok) {
        lua_pushstring(L, "movement or camera-input instruction signature mismatch");
        lua_setfield(L, -2, "Reason");
    }
    return 1;
}

static int lua_movement_enable_keyboard(lua_State *L) {
    if (!movement_build_supported()) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "keyboard movement is unavailable on this build or architecture");
        return 2;
    }

    bool combat = atomic_load_explicit(&g_combat_active,
                                       memory_order_acquire);
    if (!movement_set_patch_state(!combat)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "could not apply the requested movement state");
        return 2;
    }

    atomic_store_explicit(&g_movement_unlock_requested, true,
                          memory_order_release);
    LOG_CORE_INFO("[Movement] Keyboard movement requested%s",
                  combat ? "; suspended while combat is active" :
                           "; direct movement active and keyboard camera pan blocked");
    lua_pushboolean(L, true);
    return 1;
}

static int lua_movement_disable_keyboard(lua_State *L) {
    if (!movement_build_supported()) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "keyboard movement is unavailable on this build or architecture");
        return 2;
    }
    atomic_store_explicit(&g_movement_unlock_requested, false,
                          memory_order_release);
    if (!movement_set_patch_state(false)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "could not restore vanilla movement and camera instructions");
        return 2;
    }

    LOG_CORE_INFO("[Movement] Keyboard movement disabled; vanilla movement and camera "
                  "instructions restored");
    lua_pushboolean(L, true);
    return 1;
}

static const struct luaL_Reg movement_functions[] = {
    { "SetDirectMoveSwitch", lua_movement_set_direct_move_switch },
    { "GetCapabilities", lua_movement_get_capabilities },
    { "EnableKeyboardMovement", lua_movement_enable_keyboard },
    { "DisableKeyboardMovement", lua_movement_disable_keyboard },
    { NULL, NULL }
};

void lua_movement_register(lua_State *L, int ext_table_idx) {
    int ext = lua_absindex(L, ext_table_idx);
    lua_newtable(L);
    for (const struct luaL_Reg *fn = movement_functions; fn->name; fn++) {
        lua_pushcfunction(L, fn->func);
        lua_setfield(L, -2, fn->name);
    }
    lua_setfield(L, ext, "Movement");
}
