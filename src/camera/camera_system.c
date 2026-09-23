/**
 * camera_system.c - narrow, build-gated access to the active client camera.
 *
 * Raw camera components remain read-only through the generic ECS proxy. This
 * module owns the few semantic writes needed by camera mods and revalidates the
 * active camera immediately before every operation.
 */

#include "camera_system.h"

/* Per-store hook targets, selected at runtime. */
#include "camera_addresses.h"

#include "component_lookup.h"
#include "component_registry.h"
#include "entity_system.h"
#include "safe_memory.h"
#include "version_detect.h"
#include "logging.h"
#include "../input/input.h"
#include "../level/level_manager.h"
#include "../movement/movement_system.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wvariadic-macros"
#include <dobby.h>
#pragma clang diagnostic pop
#include <lauxlib.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define GAME_CAMERA_SIZE       0x258
#define EOC_CAMERA_SIZE        0x40

/*
 * ecl::GameCameraBehavior is the 0x258-byte CameraObject used by Native
 * Camera Tweaks. These offsets were validated live on macOS build 7398727.
 * Do not substitute the generated Windows property layout here: it labels
 * several fields after 0x50 incorrectly for the native ARM64 object.
 */
#define CAMERA_ZOOM_A_OFFSET       0x58
#define CAMERA_ZOOM_B_OFFSET       0x5c
#define CAMERA_DESIRED_ZOOM_OFFSET 0x60
#define CAMERA_TARGET_ENTITY_OFFSET 0x08
#define CAMERA_DESIRED_ROOT_OFFSET 0x24
#define CAMERA_ROTATION_OFFSET     0x74
#define CAMERA_ZOOM_DELTA_OFFSET   0xa4
#define CAMERA_MODE_FLAGS_OFFSET   0xa8
#define CAMERA_YAW_DEGREES_OFFSET  0xac
#define CAMERA_PREV_ZOOM_OFFSET    0x15c
#define CAMERA_CURRENT_ZOOM_OFFSET 0x160
#define CAMERA_CURRENT_PITCH_OFFSET 0x164
#define CAMERA_MODE_COMBAT          (1u << 0)
#define CAMERA_MODE_MOUSE_ROTATION  (1u << 8)

/* Narrow client-world components used by adaptive third-person framing. */
#define IS_SNEAKING_COMPONENT_SIZE   0x01

/* ecl::GameCameraBehavior::GetCameraPitchDegrees(bool, bool) const */

/* Native input event IDs registered by this exact game build. */
#define INPUT_CHARACTER_MOVE_FORWARD   157u
#define INPUT_CHARACTER_MOVE_BACKWARD  158u
#define INPUT_CHARACTER_MOVE_LEFT      159u
#define INPUT_CHARACTER_MOVE_RIGHT     160u
#define INPUT_TOGGLE_PRESENTATION      162u
#define INPUT_TOGGLE_SNEAK             205u

/*
 * The pointer used by GameCameraBehavior::GetCameraPitchDegrees() and
 * GetTargetFOV() to reach the camera configuration singleton. The definition
 * block offsets and the zoom field offsets below were verified from those
 * functions in the ARM64 build 7398727 disassembly.
 */
#define CAMERA_DEFINITION_EXPLORATION_OFFSET           0x7c4
#define CAMERA_DEFINITION_COMBAT_OFFSET                0x958
#define CAMERA_DEFINITION_MAX_ZOOM_OFFSET              0x28
#define CAMERA_DEFINITION_MIN_ZOOM_OFFSET              0x2c
#define CAMERA_DEFINITION_ALT_MAX_ZOOM_OFFSET          0x30
#define CAMERA_DEFINITION_ALT_MIN_ZOOM_OFFSET          0x34
#define CAMERA_DEFINITION_HORIZONTAL_OFFSET            0x64
#define CAMERA_DEFINITION_VERTICAL_OFFSET              0x68
#define CAMERA_DEFINITION_ALT_HORIZONTAL_OFFSET        0x70
#define CAMERA_DEFINITION_ALT_VERTICAL_OFFSET          0x74
#define CAMERA_DEFINITION_FOV_CLOSE_OFFSET             0x84
#define CAMERA_DEFINITION_FOV_FAR_OFFSET               0x88
#define CAMERA_DEFINITION_ALT_FOV_CLOSE_OFFSET         0x8c
#define CAMERA_DEFINITION_ALT_FOV_FAR_OFFSET           0x90
#define CAMERA_DEFINITION_TACTICAL_MIN_ZOOM_OFFSET     0xc8
#define CAMERA_DEFINITION_TACTICAL_MAX_ZOOM_OFFSET     0xcc
#define CAMERA_DEFINITION_TACTICAL_FOV_OFFSET          0xd0
#define CAMERA_DEFINITION_TARGET_FOLLOW_SPEED_OFFSET   0x118

static const uint8_t kGetPitchPrologue7398727[] = {
    0x08, 0x90, 0x47, 0x39, 0x88, 0x00, 0x00, 0x34,
    0x61, 0x00, 0x00, 0x37, 0x00, 0xe0, 0x41, 0xbd
};

static const uint8_t kGetHeightPrologue7398727[] = {
    0xff, 0x83, 0x02, 0xd1, 0xe9, 0x23, 0x06, 0x6d,
    0xf6, 0x57, 0x07, 0xa9, 0xf4, 0x4f, 0x08, 0xa9
};

static const uint8_t kInputEventPrologue7398727[] = {
    0xfa, 0x67, 0xbb, 0xa9, 0xf8, 0x5f, 0x01, 0xa9,
    0xf6, 0x57, 0x02, 0xa9, 0xf4, 0x4f, 0x03, 0xa9
};

static const uint8_t kGetRotatedInputPrologue7398727[] = {
    0xeb, 0x2b, 0xbb, 0x6d, 0xe9, 0x23, 0x01, 0x6d,
    0xf6, 0x57, 0x02, 0xa9, 0xf4, 0x4f, 0x03, 0xa9
};

static const uint8_t kGetDarknessComponentPrologue7398727[] = {
    0xff, 0x43, 0x01, 0xd1, 0xf6, 0x57, 0x02, 0xa9,
    0xf4, 0x4f, 0x03, 0xa9, 0xfd, 0x7b, 0x04, 0xa9
};

static const uint8_t kGameInputEventPrologue7398727[] = {
    0xeb, 0x2b, 0xb8, 0x6d, 0xe9, 0x23, 0x01, 0x6d,
    0xfc, 0x6f, 0x02, 0xa9, 0xfa, 0x67, 0x03, 0xa9
};

static const uint8_t kAppInputEventPrologue7398727[] = {
    0xff, 0x43, 0x01, 0xd1, 0xf6, 0x57, 0x02, 0xa9,
    0xf4, 0x4f, 0x03, 0xa9, 0xfd, 0x7b, 0x04, 0xa9
};

static const uint8_t kInputManagerGetValuePrologue7398727[] = {
    0x5f, 0x04, 0x00, 0x31, 0x20, 0x08, 0x00, 0x54,
    0x08, 0xd0, 0x22, 0x8b, 0x08, 0x1d, 0x41, 0xb9
};

typedef struct {
    float x;
    float y;
    float z;
} CameraVector3;

typedef float (*CameraGetPitchFn)(void *camera, bool force_current,
                                  bool use_target);
typedef float (*AiGridGetHeightInAreaFn)(void *aigrid,
                                         const CameraVector3 *position,
                                         float radius);
typedef uint64_t (*InputControllerOnInputEventFn)(void *controller,
                                                   const void *event);
typedef CameraVector3 (*GetRotatedInputFn)(int16_t player_index,
                                            bool camera_relative);
typedef const void *(*GetDarknessComponentFn)(void *world,
                                               uint64_t entity_handle);
typedef uint64_t (*GameInputOnInputEventFn)(void *game_input,
                                             const void *event);
typedef bool (*AppOnInputEventFn)(void *app, const void *event);
typedef uint64_t (*InputManagerGetInputValueFn)(void *input_manager,
                                                 uint32_t command,
                                                 int32_t player_index);

static CameraGetPitchFn g_original_get_pitch;
static bool g_pitch_hook_installed;
static bool g_pitch_hook_attempted;
static _Atomic(bool) g_pitch_override_enabled;
static _Atomic(uint32_t) g_pitch_override_bits;
static _Atomic(bool) g_mouse_pitch_enabled;
static _Atomic(int_fast64_t) g_mouse_delta_x;
static _Atomic(int_fast64_t) g_mouse_delta_y;
static _Atomic(uint32_t) g_mouse_pitch_min_bits;
static _Atomic(uint32_t) g_mouse_pitch_max_bits;
static _Atomic(uint32_t) g_mouse_pitch_sensitivity_bits;
static _Atomic(bool) g_mouse_pitch_invert;
static _Atomic(bool) g_caps_lock_mouse_look_enabled;
static _Atomic(bool) g_caps_lock_mouse_look_active;
static AiGridGetHeightInAreaFn g_aigrid_get_height;
static bool g_floor_query_attempted;
static _Atomic(bool) g_floor_protection_enabled;
static _Atomic(uint32_t) g_floor_offset_bits;
static _Atomic(uint32_t) g_floor_min_zoom_bits;
static _Atomic(uint32_t) g_floor_radius_bits;
static _Atomic(bool) g_zoom_toggle_enabled;
static _Atomic(uint32_t) g_zoom_toggle_close_bits;
static _Atomic(uint32_t) g_zoom_toggle_far_bits;
static _Atomic(uint32_t) g_zoom_toggle_selected_bits;
static _Atomic(uint32_t) g_zoom_toggle_smooth_time_bits;
static _Atomic(uint64_t) g_zoom_toggle_last_time_ns;
static _Atomic(bool) g_zoom_toggle_invert;
static _Atomic(bool) g_profile_wheel_enabled;
static _Atomic(int) g_profile_wheel_pending;
static _Atomic(uint64_t) g_profile_wheel_last_time_ns;
static _Atomic(bool) g_combat_mode_active;
static _Atomic(bool) g_adaptive_enabled;
static _Atomic(bool) g_adaptive_crouch_enabled;
static _Atomic(bool) g_adaptive_crouching;
static _Atomic(bool) g_adaptive_component_crouching;
static _Atomic(bool) g_adaptive_running;
static _Atomic(bool) g_adaptive_crouch_fallback;
static _Atomic(uint32_t) g_adaptive_speed_bits;
static _Atomic(uint32_t) g_adaptive_crouch_mix_bits;
static _Atomic(uint32_t) g_adaptive_run_mix_bits;
static _Atomic(uint32_t) g_adaptive_crouch_delta_bits;
static _Atomic(uint32_t) g_adaptive_crouch_smooth_time_bits;
static _Atomic(uint32_t) g_adaptive_run_fov_boost_bits;
static _Atomic(uint32_t) g_adaptive_run_speed_threshold_bits;
static _Atomic(uint32_t) g_adaptive_run_smooth_time_bits;
static _Atomic(uint64_t) g_adaptive_last_time_ns;
static atomic_flag g_adaptive_lock = ATOMIC_FLAG_INIT;
static InputControllerOnInputEventFn g_original_input_event;
static bool g_input_event_hook_installed;
static bool g_input_event_hook_attempted;
static _Atomic(uint64_t) g_rotated_input_calls;  /* diagnostic: is the task running? */
static _Atomic(uint32_t) g_input_movement_mask;
static _Atomic(bool) g_input_sneak_down;
static _Atomic(uint64_t) g_input_target_handle;
static _Atomic(uintptr_t) g_input_entity_world;
static _Atomic(uint32_t) g_input_last_command;
static _Atomic(bool) g_input_last_active;
static GetRotatedInputFn g_original_get_rotated_input;
static bool g_rotated_input_hook_installed;
static bool g_rotated_input_hook_attempted;
static _Atomic(uint32_t) g_native_move_magnitude_bits;
static GetDarknessComponentFn g_get_darkness_component;
static bool g_darkness_getter_attempted;
static GameInputOnInputEventFn g_original_game_input_event;
static bool g_game_input_hook_installed;
static bool g_game_input_hook_attempted;
static _Atomic(uint32_t) g_game_input_last_command;
static _Atomic(bool) g_game_input_last_active;
static AppOnInputEventFn g_original_app_input_event;
static bool g_app_input_hook_installed;
static bool g_app_input_hook_attempted;
static _Atomic(uintptr_t) g_app_instance;
static _Atomic(bool) g_presentation_policy_enabled;
static _Atomic(bool) g_presentation_tracked_hidden;
static _Atomic(bool) g_presentation_managed_active;
static _Atomic(bool) g_presentation_restore_hidden;
static InputManagerGetInputValueFn g_input_manager_get_value;
static void *g_input_manager;
static bool g_input_value_attempted;
static _Atomic(uint32_t) g_hide_action_value_bits;
static _Atomic(uint32_t) g_hide_action_toggle_count;

static void apply_adaptive_camera(void *camera);
static bool install_input_event_hook(void);
static bool install_rotated_input_hook(void);
static bool resolve_darkness_getter(void);
static bool install_game_input_event_hook(void);
static bool install_app_input_event_hook(void);
static bool resolve_input_value_reader(void);
static float atomic_float_load(const _Atomic(uint32_t) *storage);
static void atomic_float_store(_Atomic(uint32_t) *storage, float value);

static bool toggle_tracked_presentation(void) {
    bool current = atomic_load_explicit(&g_presentation_tracked_hidden,
                                        memory_order_acquire);
    while (!atomic_compare_exchange_weak_explicit(
            &g_presentation_tracked_hidden, &current, !current,
            memory_order_acq_rel, memory_order_acquire)) {
    }
    return !current;
}

bool camera_get_game_ui_hidden(bool *hidden, const char **reason) {
    if (!hidden) {
        if (reason) *reason = "output pointer is required";
        return false;
    }
    if (!g_app_input_hook_installed) {
        if (reason) *reason = "BG3 presentation input is not available yet";
        return false;
    }
    *hidden = atomic_load_explicit(&g_presentation_tracked_hidden,
                                   memory_order_acquire);
    if (reason) *reason = NULL;
    return true;
}

bool camera_set_hide_ui_with_mouse_look(bool enabled, const char **reason) {
    if (!g_app_input_hook_installed && !install_app_input_event_hook()) {
        if (reason) *reason = "BG3 presentation input is not available";
        return false;
    }
    atomic_store_explicit(&g_presentation_policy_enabled, enabled,
                          memory_order_release);
    if (reason) *reason = NULL;
    return true;
}

static bool invoke_toggle_presentation(void) {
    void *app = (void *)atomic_load_explicit(&g_app_instance,
                                             memory_order_acquire);
    if (!app || !g_original_app_input_event) return false;

    uint8_t event[0x20] = {0};
    uint32_t command = INPUT_TOGGLE_PRESENTATION;
    memcpy(event, &command, sizeof(command));
    g_original_app_input_event(app, event);
    bool hidden = toggle_tracked_presentation();
    LOG_CORE_INFO("[UI] Game UI %s through TogglePresentation",
                  hidden ? "hidden" : "restored");
    return true;
}

static void update_presentation_mode(bool caps_mouse_look, bool combat) {
    bool policy = atomic_load_explicit(&g_presentation_policy_enabled,
                                       memory_order_acquire);
    bool manage = policy && caps_mouse_look && !combat;
    bool active = atomic_load_explicit(&g_presentation_managed_active,
                                       memory_order_acquire);
    bool hidden = atomic_load_explicit(&g_presentation_tracked_hidden,
                                       memory_order_acquire);

    if (manage && !active) {
        atomic_store_explicit(&g_presentation_restore_hidden, hidden,
                              memory_order_release);
        atomic_store_explicit(&g_presentation_managed_active, true,
                              memory_order_release);
        if (!hidden && !invoke_toggle_presentation()) {
            atomic_store_explicit(&g_presentation_managed_active, false,
                                  memory_order_release);
        }
    } else if (manage && !hidden) {
        /* Caps Lock is authoritative while this profile opts into hiding. */
        invoke_toggle_presentation();
    } else if (!manage && active) {
        bool restore = atomic_load_explicit(&g_presentation_restore_hidden,
                                            memory_order_acquire);
        if (hidden != restore && !invoke_toggle_presentation()) return;
        atomic_store_explicit(&g_presentation_managed_active, false,
                              memory_order_release);
    }
}

/*
 * Poll Hide from the camera update rather than the movement controller.
 * CharacterTask_MoveController can stop updating while actions such as Shove
 * run, which otherwise lets a Hide transition pass without being observed.
 */
static void poll_hide_action(void) {
    if (!g_input_manager || !g_input_manager_get_value) return;

    uint64_t packed = g_input_manager_get_value(
        g_input_manager, INPUT_TOGGLE_SNEAK, 0);
    uint32_t value_bits = (uint32_t)(packed >> 32);
    float value = 0.0f;
    memcpy(&value, &value_bits, sizeof(value));
    if (!isfinite(value)) value = 0.0f;
    atomic_float_store(&g_hide_action_value_bits, value);

    bool down = fabsf(value) > 0.5f;
    bool was_down = atomic_exchange_explicit(&g_input_sneak_down, down,
                                               memory_order_acq_rel);
    if (down && !was_down) {
        bool current = atomic_load_explicit(
            &g_adaptive_crouch_fallback, memory_order_relaxed);
        atomic_store_explicit(&g_adaptive_crouch_fallback, !current,
                              memory_order_relaxed);
        atomic_fetch_add_explicit(&g_hide_action_toggle_count, 1,
                                  memory_order_relaxed);
    }
}

static void adaptive_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_adaptive_lock,
                                              memory_order_acquire)) {
    }
}

static void adaptive_unlock(void) {
    atomic_flag_clear_explicit(&g_adaptive_lock, memory_order_release);
}

static bool camera_is_combat(void *camera) {
    if (!camera) return false;
    uint32_t mode_flags = 0;
    memcpy(&mode_flags, (char *)camera + CAMERA_MODE_FLAGS_OFFSET,
           sizeof(mode_flags));
    return (mode_flags & CAMERA_MODE_COMBAT) != 0;
}

typedef struct {
    float min;
    float max;
    float tactical_min;
    float tactical_max;
    float alt_min;
    float alt_max;
} CameraZoomLimits;

typedef struct {
    float close;
    float far;
    float tactical;
    float alt_close;
    float alt_far;
} CameraFOVSettings;

typedef struct {
    float horizontal;
    float vertical;
    float alt_horizontal;
    float alt_vertical;
} CameraOffsetSettings;

typedef struct {
    float speed;
} CameraFollowSettings;

typedef struct {
    void *singleton;
    void *exploration;
    void *combat;
    CameraZoomLimits exploration_limits;
    CameraZoomLimits combat_limits;
    CameraFOVSettings exploration_fov;
    CameraFOVSettings combat_fov;
    CameraOffsetSettings exploration_offsets;
    CameraOffsetSettings combat_offsets;
    CameraFollowSettings exploration_follow;
    CameraFollowSettings combat_follow;
} CameraDefinitionState;

static bool g_zoom_snapshot_valid;
static bool g_zoom_override_enabled;
static CameraDefinitionState g_zoom_snapshot;
static bool g_fov_snapshot_valid;
static bool g_fov_override_enabled;
static CameraDefinitionState g_fov_snapshot;
static bool g_offset_snapshot_valid;
static bool g_offset_override_enabled;
static CameraDefinitionState g_offset_snapshot;
static bool g_follow_snapshot_valid;
static bool g_follow_override_enabled;
static CameraDefinitionState g_follow_snapshot;

static float atomic_float_load(const _Atomic(uint32_t) *storage) {
    uint32_t bits = atomic_load_explicit(storage, memory_order_relaxed);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void atomic_float_store(_Atomic(uint32_t) *storage, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    atomic_store_explicit(storage, bits, memory_order_relaxed);
}

static float pitch_override_value(void) {
    return atomic_float_load(&g_pitch_override_bits);
}

static bool resolve_floor_query(void) {
    if (g_aigrid_get_height) return true;
    if (g_floor_query_attempted || !version_detect_addresses_safe()) {
        return false;
    }

    void *base = version_detect_get_binary_base();
    if (!base) return false;

    g_floor_query_attempted = true;
    void *target = (char *)base + AIGRID_GET_HEIGHT_OFFSET_7398727;
    uint8_t observed[sizeof(kGetHeightPrologue7398727)];
    if (!safe_memory_read((mach_vm_address_t)target, observed,
                          sizeof(observed)) ||
        memcmp(observed, kGetHeightPrologue7398727,
               sizeof(observed)) != 0) {
        LOG_CORE_ERROR("[Camera] Floor protection disabled: AiGrid::GetHeightInArea signature mismatch");
        return false;
    }

    g_aigrid_get_height = (AiGridGetHeightInAreaFn)target;
    LOG_CORE_INFO("[Camera] AiGrid::GetHeightInArea resolved at %p", target);
    return true;
}

static bool read_camera_vector(void *camera, size_t offset,
                               CameraVector3 *out) {
    return camera && out && safe_memory_read(
        (mach_vm_address_t)((char *)camera + offset), out, sizeof(*out));
}

static bool plausible_camera_vector(const CameraVector3 *value) {
    return isfinite(value->x) && isfinite(value->y) && isfinite(value->z) &&
        fabsf(value->x) < 1000000.0f && fabsf(value->y) < 1000000.0f &&
        fabsf(value->z) < 1000000.0f;
}

static void apply_floor_protection(void *camera) {
    if (!camera || !g_aigrid_get_height ||
        !atomic_load_explicit(&g_floor_protection_enabled,
                              memory_order_relaxed)) {
        return;
    }

    CameraVector3 root;
    CameraVector3 rotation;
    float zoom;
    if (!read_camera_vector(camera, CAMERA_DESIRED_ROOT_OFFSET, &root) ||
        !read_camera_vector(camera, CAMERA_ROTATION_OFFSET, &rotation) ||
        !safe_memory_read((mach_vm_address_t)((char *)camera +
                              CAMERA_ZOOM_B_OFFSET), &zoom, sizeof(zoom)) ||
        !plausible_camera_vector(&root) ||
        !plausible_camera_vector(&rotation) || !isfinite(zoom)) {
        return;
    }

    float min_zoom = atomic_float_load(&g_floor_min_zoom_bits);
    if (zoom <= min_zoom || zoom > 100.0f || rotation.y >= -0.0001f) {
        return;
    }

    void *aigrid = level_get_client_aigrid();
    if (!aigrid) return;

    float floor_offset = atomic_float_load(&g_floor_offset_bits);
    float radius = atomic_float_load(&g_floor_radius_bits);
    float adjusted = zoom;

    /* Re-query after changing the distance because the camera's X/Z point can
     * cross onto a different tile. Three passes converge without the hundreds
     * of 0.01-distance queries used by the original Windows implementation. */
    for (int pass = 0; pass < 3; pass++) {
        CameraVector3 candidate = {
            root.x + rotation.x * adjusted,
            root.y + rotation.y * adjusted,
            root.z + rotation.z * adjusted,
        };
        float floor_height = g_aigrid_get_height(aigrid, &candidate, radius);
        if (!isfinite(floor_height) || fabsf(floor_height) >= 1000000.0f) {
            return;
        }

        float required_y = floor_height + floor_offset;
        if (candidate.y >= required_y) break;

        float safe_zoom = (required_y - root.y) / rotation.y;
        if (!isfinite(safe_zoom)) return;
        safe_zoom = fmaxf(min_zoom, fminf(adjusted, safe_zoom));
        if (safe_zoom >= adjusted - 0.001f) {
            safe_zoom = fmaxf(min_zoom, adjusted - 0.05f);
        }
        adjusted = safe_zoom;
    }

    if (adjusted < zoom - 0.001f) {
        safe_memory_write((mach_vm_address_t)((char *)camera +
                              CAMERA_ZOOM_A_OFFSET), &adjusted,
                          sizeof(adjusted));
        safe_memory_write((mach_vm_address_t)((char *)camera +
                              CAMERA_ZOOM_B_OFFSET), &adjusted,
                          sizeof(adjusted));
    }
}

static void apply_zoom_toggle(void *camera) {
    if (!camera || !atomic_load_explicit(&g_zoom_toggle_enabled,
                                         memory_order_acquire) ||
        camera_is_combat(camera)) {
        return;
    }

    float selected = atomic_float_load(&g_zoom_toggle_selected_bits);
    if (!isfinite(selected) || selected < 0.1f || selected > 100.0f) {
        return;
    }

    float current;
    memcpy(&current, (char *)camera + CAMERA_CURRENT_ZOOM_OFFSET,
           sizeof(current));
    float next = selected;
    float smooth_time = atomic_float_load(&g_zoom_toggle_smooth_time_bits);
    if (isfinite(current) && current >= 0.1f && current <= 100.0f &&
        isfinite(smooth_time) && smooth_time > 0.001f) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ULL +
            (uint64_t)now.tv_nsec;
        uint64_t previous_ns = atomic_exchange_explicit(
            &g_zoom_toggle_last_time_ns, now_ns, memory_order_relaxed);
        float dt = previous_ns > 0 && now_ns > previous_ns ?
            (float)(now_ns - previous_ns) / 1000000000.0f : 1.0f / 60.0f;
        dt = fminf(0.1f, fmaxf(0.0f, dt));
        /* SmoothTime is approximately the visible transition duration: three
         * time constants reach 95% of the selected distance. */
        float alpha = 1.0f - expf(-3.0f * dt / smooth_time);
        next = current + (selected - current) * alpha;
        if (fabsf(selected - next) < 0.01f) next = selected;
    }

    const size_t offsets[] = {
        CAMERA_ZOOM_A_OFFSET,
        CAMERA_ZOOM_B_OFFSET,
        CAMERA_PREV_ZOOM_OFFSET,
        CAMERA_CURRENT_ZOOM_OFFSET,
    };
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        float observed;
        memcpy(&observed, (char *)camera + offsets[i], sizeof(observed));
        if (!isfinite(observed) || fabsf(observed - next) > 0.001f) {
            memcpy((char *)camera + offsets[i], &next, sizeof(next));
        }
    }
}

void camera_input_mouse_delta(double delta_x, double delta_y) {
    if (!atomic_load_explicit(&g_mouse_pitch_enabled, memory_order_relaxed) ||
        atomic_load_explicit(&g_combat_mode_active, memory_order_acquire) ||
        !isfinite(delta_x) || !isfinite(delta_y)) {
        return;
    }

    if (delta_x > 10000.0) delta_x = 10000.0;
    if (delta_x < -10000.0) delta_x = -10000.0;
    if (delta_y > 10000.0) delta_y = 10000.0;
    if (delta_y < -10000.0) delta_y = -10000.0;
    atomic_fetch_add_explicit(&g_mouse_delta_x, (int_fast64_t)delta_x,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&g_mouse_delta_y, (int_fast64_t)delta_y,
                              memory_order_relaxed);
}

void camera_input_scroll(double delta_y) {
    if (atomic_load_explicit(&g_combat_mode_active, memory_order_acquire) ||
        !isfinite(delta_y) || fabs(delta_y) < 0.001) {
        return;
    }

    if (atomic_load_explicit(&g_profile_wheel_enabled,
                             memory_order_acquire)) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ULL +
            (uint64_t)now.tv_nsec;
        uint64_t previous_ns = atomic_load_explicit(
            &g_profile_wheel_last_time_ns, memory_order_relaxed);
        if (previous_ns > 0 && now_ns - previous_ns < 140000000ULL) return;

        int expected = 0;
        int direction = delta_y > 0.0 ? -1 : 1;
        if (atomic_compare_exchange_strong_explicit(
                &g_profile_wheel_pending, &expected, direction,
                memory_order_acq_rel, memory_order_relaxed)) {
            atomic_store_explicit(&g_profile_wheel_last_time_ns, now_ns,
                                  memory_order_relaxed);
        }
        return;
    }

    if (!atomic_load_explicit(&g_zoom_toggle_enabled,
                              memory_order_acquire)) return;

    bool select_close = delta_y > 0.0;
    if (atomic_load_explicit(&g_zoom_toggle_invert, memory_order_relaxed)) {
        select_close = !select_close;
    }
    float selected = atomic_float_load(select_close ?
        &g_zoom_toggle_close_bits : &g_zoom_toggle_far_bits);
    atomic_float_store(&g_zoom_toggle_selected_bits, selected);
}

static float camera_get_pitch_hook(void *camera, bool force_current,
                                   bool use_target) {
    bool combat = camera_is_combat(camera);
    bool caps_mouse_look = camera && !combat &&
        atomic_load_explicit(&g_caps_lock_mouse_look_enabled,
                             memory_order_acquire) &&
        input_caps_lock_active();
    bool was_caps_mouse_look = atomic_exchange_explicit(
        &g_caps_lock_mouse_look_active, caps_mouse_look,
        memory_order_acq_rel);
    if (was_caps_mouse_look && !caps_mouse_look) {
        input_set_relative_mouse_mode(false);
    } else if (!was_caps_mouse_look && caps_mouse_look) {
        input_set_relative_mouse_mode(true);
    }
    uint32_t mode_flags = 0;

    if (camera) {
        memcpy(&mode_flags, (char *)camera + CAMERA_MODE_FLAGS_OFFSET,
               sizeof(mode_flags));
    }

    float vanilla = g_original_get_pitch(camera, force_current, use_target);

    atomic_store_explicit(&g_combat_mode_active, combat,
                          memory_order_release);
    movement_update_combat_state(combat);
    update_presentation_mode(caps_mouse_look, combat);
    if (combat) {
        atomic_store_explicit(&g_mouse_delta_x, 0, memory_order_relaxed);
        atomic_store_explicit(&g_mouse_delta_y, 0, memory_order_relaxed);
        atomic_store_explicit(&g_zoom_toggle_last_time_ns, 0,
                              memory_order_relaxed);
        return vanilla;
    }
    apply_adaptive_camera(camera);
    apply_zoom_toggle(camera);
    if (camera && atomic_load_explicit(&g_pitch_override_enabled,
                                      memory_order_acquire)) {
        float pitch = pitch_override_value();
        if (atomic_load_explicit(&g_mouse_pitch_enabled,
                                 memory_order_relaxed)) {
            int_fast64_t delta_x = atomic_exchange_explicit(
                &g_mouse_delta_x, 0, memory_order_relaxed);
            int_fast64_t delta = atomic_exchange_explicit(
                &g_mouse_delta_y, 0, memory_order_relaxed);
            if (caps_mouse_look && delta_x != 0) {
                float yaw = 0.0f;
                memcpy(&yaw, (char *)camera + CAMERA_YAW_DEGREES_OFFSET,
                       sizeof(yaw));
                if (isfinite(yaw)) {
                    yaw += (float)delta_x * atomic_float_load(
                        &g_mouse_pitch_sensitivity_bits);
                    yaw = fmodf(yaw, 360.0f);
                    if (yaw < 0.0f) yaw += 360.0f;
                    safe_memory_write((mach_vm_address_t)((char *)camera +
                                          CAMERA_YAW_DEGREES_OFFSET),
                                      &yaw, sizeof(yaw));
                }
            }
            if (delta != 0 && (caps_mouse_look ||
                               (mode_flags & CAMERA_MODE_MOUSE_ROTATION))) {
                float sensitivity = atomic_float_load(
                    &g_mouse_pitch_sensitivity_bits);
                float direction = atomic_load_explicit(&g_mouse_pitch_invert,
                    memory_order_relaxed) ? -1.0f : 1.0f;
                pitch += (float)delta * sensitivity * direction;
                float min_pitch = atomic_float_load(&g_mouse_pitch_min_bits);
                float max_pitch = atomic_float_load(&g_mouse_pitch_max_bits);
                pitch = fminf(max_pitch, fmaxf(min_pitch, pitch));
                atomic_float_store(&g_pitch_override_bits, pitch);
            }
        }
        apply_floor_protection(camera);
        return pitch;
    }
    return vanilla;
}

static bool install_pitch_hook(void) {
    if (g_pitch_hook_attempted) return g_pitch_hook_installed;

    if (!version_detect_addresses_safe()) {
        LOG_CORE_DEBUG("[Camera] Pitch hook deferred: build not verified yet");
        return false;
    }

    void *base = version_detect_get_binary_base();
    if (!base) {
        LOG_CORE_DEBUG("[Camera] Pitch hook deferred: game base unavailable");
        return false;
    }

    /* From this point onward a failure is definitive for this process. */
    g_pitch_hook_attempted = true;
    void *target = (char *)base + CAMERA_GET_PITCH_OFFSET_7398727;
    uint8_t observed[sizeof(kGetPitchPrologue7398727)];
    if (!safe_memory_read((mach_vm_address_t)target, observed,
                          sizeof(observed)) ||
        memcmp(observed, kGetPitchPrologue7398727,
               sizeof(observed)) != 0) {
        LOG_CORE_ERROR("[Camera] Pitch hook disabled: GetCameraPitchDegrees signature mismatch");
        return false;
    }

    int result = DobbyHook(target, (void *)camera_get_pitch_hook,
                           (void **)&g_original_get_pitch);
    if (result != 0 || !g_original_get_pitch) {
        LOG_CORE_ERROR("[Camera] Failed to hook GetCameraPitchDegrees (Dobby=%d)",
                       result);
        return false;
    }

    g_pitch_hook_installed = true;
    LOG_CORE_INFO("[Camera] GetCameraPitchDegrees hook installed at %p", target);
    return true;
}

/*
 * Observe the same semantic input events BG3 sends to its active character
 * controller. This is binding-aware and runs on the game's input path; it is
 * deliberately limited to the four direct-movement commands and ToggleSneak.
 * The controller also supplies the exact selected-character entity handle at
 * +0x08, avoiding camera-target layout guesses.
 */
static uint64_t input_controller_on_event_hook(void *controller,
                                                const void *event) {
    if (controller && event) {
        uint32_t command = 0;
        uint8_t active = 0;
        uint64_t target = 0;
        uintptr_t world = 0;
        memcpy(&command, event, sizeof(command));
        memcpy(&active, (const char *)event + 0x1c, sizeof(active));
        memcpy(&target, (const char *)controller + 0x08, sizeof(target));
        memcpy(&world, (const char *)controller + 0x10, sizeof(world));

        atomic_store_explicit(&g_input_last_command, command,
                              memory_order_relaxed);
        atomic_store_explicit(&g_input_last_active, active != 0,
                              memory_order_relaxed);

        /* InputController owns the exact client EntityWorld/character pair. */
        if (target != 0 && world != 0) {
            atomic_store_explicit(&g_input_entity_world, world,
                                  memory_order_release);
            atomic_store_explicit(&g_input_target_handle, target,
                                  memory_order_release);
        }

        if (command >= INPUT_CHARACTER_MOVE_FORWARD &&
            command <= INPUT_CHARACTER_MOVE_RIGHT) {
            uint32_t bit = 1u << (command - INPUT_CHARACTER_MOVE_FORWARD);
            if (active) {
                atomic_fetch_or_explicit(&g_input_movement_mask, bit,
                                         memory_order_relaxed);
            } else {
                atomic_fetch_and_explicit(&g_input_movement_mask, ~bit,
                                          memory_order_relaxed);
            }
        }
    }

    return g_original_input_event(controller, event);
}

/*
 * This is BG3's own binding-aware movement-vector calculation, called from
 * CharacterTask_MoveController::Update. Its returned vector reaches zero on
 * stop, unlike the persistent flags carried by InputEvent +0x1c.
 */
static CameraVector3 get_rotated_input_hook(int16_t player_index,
                                             bool camera_relative) {
    atomic_fetch_add_explicit(&g_rotated_input_calls, 1, memory_order_relaxed);
    CameraVector3 movement = g_original_get_rotated_input(player_index,
                                                           camera_relative);
    float magnitude = sqrtf(movement.x * movement.x +
                            movement.y * movement.y +
                            movement.z * movement.z);
    if (!isfinite(magnitude)) magnitude = 0.0f;
    atomic_float_store(&g_native_move_magnitude_bits, magnitude);

    return movement;
}

static bool install_input_event_hook(void) {
    if (g_input_event_hook_attempted) return g_input_event_hook_installed;
    if (!version_detect_addresses_safe()) {
        LOG_CORE_DEBUG("[Camera] Native input hook deferred: build not verified yet");
        return false;
    }

    void *base = version_detect_get_binary_base();
    if (!base) {
        LOG_CORE_DEBUG("[Camera] Native input hook deferred: game base unavailable");
        return false;
    }

    g_input_event_hook_attempted = true;
    void *target = (char *)base + INPUT_CONTROLLER_ON_EVENT_OFFSET_7398727;
    uint8_t observed[sizeof(kInputEventPrologue7398727)];
    if (!safe_memory_read((mach_vm_address_t)target, observed,
                          sizeof(observed)) ||
        memcmp(observed, kInputEventPrologue7398727,
               sizeof(observed)) != 0) {
        LOG_CORE_ERROR("[Camera] Native input disabled: OnInputEvent signature mismatch");
        return false;
    }

    int result = DobbyHook(target, (void *)input_controller_on_event_hook,
                           (void **)&g_original_input_event);
    if (result != 0 || !g_original_input_event) {
        LOG_CORE_ERROR("[Camera] Failed to hook InputController::OnInputEvent (Dobby=%d)",
                       result);
        return false;
    }

    g_input_event_hook_installed = true;
    LOG_CORE_INFO("[Camera] Native character input hook installed at %p", target);
    return true;
}

static bool install_rotated_input_hook(void) {
    if (g_rotated_input_hook_attempted) return g_rotated_input_hook_installed;
    if (!version_detect_addresses_safe()) {
        LOG_CORE_DEBUG("[Camera] Movement-vector hook deferred: build not verified yet");
        return false;
    }

    void *base = version_detect_get_binary_base();
    if (!base) {
        LOG_CORE_DEBUG("[Camera] Movement-vector hook deferred: game base unavailable");
        return false;
    }

    g_rotated_input_hook_attempted = true;
    void *target = (char *)base + GET_ROTATED_INPUT_OFFSET_7398727;
    uint8_t observed[sizeof(kGetRotatedInputPrologue7398727)];
    if (!safe_memory_read((mach_vm_address_t)target, observed,
                          sizeof(observed)) ||
        memcmp(observed, kGetRotatedInputPrologue7398727,
               sizeof(observed)) != 0) {
        LOG_CORE_ERROR("[Camera] Movement-vector hook disabled: GetRotatedInput signature mismatch");
        return false;
    }

    int result = DobbyHook(target, (void *)get_rotated_input_hook,
                           (void **)&g_original_get_rotated_input);
    if (result != 0 || !g_original_get_rotated_input) {
        LOG_CORE_ERROR("[Camera] Failed to hook GetRotatedInput (Dobby=%d)",
                       result);
        return false;
    }

    g_rotated_input_hook_installed = true;
    LOG_CORE_INFO("[Camera] Movement-vector hook installed at %p", target);
    return true;
}

static bool resolve_darkness_getter(void) {
    if (g_darkness_getter_attempted) {
        return g_get_darkness_component != NULL;
    }
    if (!version_detect_addresses_safe()) return false;

    void *base = version_detect_get_binary_base();
    if (!base) return false;

    g_darkness_getter_attempted = true;
    void *target = (char *)base +
        GET_DARKNESS_COMPONENT_OFFSET_7398727;
    uint8_t observed[sizeof(kGetDarknessComponentPrologue7398727)];
    if (!safe_memory_read((mach_vm_address_t)target, observed,
                          sizeof(observed)) ||
        memcmp(observed, kGetDarknessComponentPrologue7398727,
               sizeof(observed)) != 0) {
        LOG_CORE_ERROR("[Camera] Typed Hide-state lookup disabled: signature mismatch");
        return false;
    }

    g_get_darkness_component = (GetDarknessComponentFn)target;
    LOG_CORE_INFO("[Camera] Typed DarknessComponent lookup resolved at %p",
                  target);
    return true;
}

/* ToggleSneak is handled above InputController, in the global GameInput path. */
static uint64_t game_input_on_event_hook(void *game_input,
                                          const void *event) {
    if (event) {
        uint32_t command = 0;
        uint8_t active = 0;
        memcpy(&command, event, sizeof(command));
        memcpy(&active, (const char *)event + 0x1c, sizeof(active));
        atomic_store_explicit(&g_game_input_last_command, command,
                              memory_order_relaxed);
        atomic_store_explicit(&g_game_input_last_active, active != 0,
                              memory_order_relaxed);
    }
    return g_original_game_input_event(game_input, event);
}

static bool install_game_input_event_hook(void) {
    if (g_game_input_hook_attempted) return g_game_input_hook_installed;
    if (!version_detect_addresses_safe()) {
        LOG_CORE_DEBUG("[Camera] GameInput hook deferred: build not verified yet");
        return false;
    }

    void *base = version_detect_get_binary_base();
    if (!base) return false;
    g_game_input_hook_attempted = true;
    void *target = (char *)base + GAME_INPUT_ON_EVENT_OFFSET_7398727;
    uint8_t observed[sizeof(kGameInputEventPrologue7398727)];
    if (!safe_memory_read((mach_vm_address_t)target, observed,
                          sizeof(observed)) ||
        memcmp(observed, kGameInputEventPrologue7398727,
               sizeof(observed)) != 0) {
        LOG_CORE_ERROR("[Camera] GameInput hook disabled: signature mismatch");
        return false;
    }

    int result = DobbyHook(target, (void *)game_input_on_event_hook,
                           (void **)&g_original_game_input_event);
    if (result != 0 || !g_original_game_input_event) {
        LOG_CORE_ERROR("[Camera] Failed to hook GameInput::OnInputEvent (Dobby=%d)",
                       result);
        return false;
    }
    g_game_input_hook_installed = true;
    LOG_CORE_INFO("[Camera] Global semantic input hook installed at %p", target);
    return true;
}

static bool app_on_input_event_hook(void *app, const void *event) {
    if (app) {
        atomic_store_explicit(&g_app_instance, (uintptr_t)app,
                              memory_order_release);
    }
    uint32_t command = 0;
    if (event) memcpy(&command, event, sizeof(command));

    bool handled = g_original_app_input_event(app, event);
    if (command == INPUT_TOGGLE_PRESENTATION) {
        /* Calls made by update_presentation_mode use the original trampoline
         * directly. Reaching this hook therefore means BG3/user input toggled
         * presentation mode, and we keep our parity tracker synchronized. */
        toggle_tracked_presentation();
    }
    return handled;
}

static bool install_app_input_event_hook(void) {
    if (g_app_input_hook_attempted) return g_app_input_hook_installed;
    if (!version_detect_addresses_safe()) {
        LOG_CORE_DEBUG("[UI] App input hook deferred: build not verified yet");
        return false;
    }
    void *base = version_detect_get_binary_base();
    if (!base) return false;

    g_app_input_hook_attempted = true;
    void *target = (char *)base + APP_ON_INPUT_EVENT_OFFSET_7398727;
    uint8_t observed[sizeof(kAppInputEventPrologue7398727)];
    if (!safe_memory_read((mach_vm_address_t)target, observed,
                          sizeof(observed)) ||
        memcmp(observed, kAppInputEventPrologue7398727,
               sizeof(observed)) != 0) {
        LOG_CORE_ERROR("[UI] Presentation control disabled: App::OnInputEvent signature mismatch");
        return false;
    }

    int result = DobbyHook(target, (void *)app_on_input_event_hook,
                           (void **)&g_original_app_input_event);
    if (result != 0 || !g_original_app_input_event) {
        LOG_CORE_ERROR("[UI] Failed to hook App::OnInputEvent (Dobby=%d)",
                       result);
        return false;
    }
    g_app_input_hook_installed = true;
    LOG_CORE_INFO("[UI] TogglePresentation hook installed at %p", target);
    return true;
}

static bool resolve_input_value_reader(void) {
    if (g_input_value_attempted) {
        return g_input_manager && g_input_manager_get_value;
    }
    if (!version_detect_addresses_safe()) return false;

    void *base = version_detect_get_binary_base();
    if (!base) return false;
    g_input_value_attempted = true;
    void *target = (char *)base + INPUT_MANAGER_GET_VALUE_OFFSET_7398727;
    uint8_t observed[sizeof(kInputManagerGetValuePrologue7398727)];
    if (!safe_memory_read((mach_vm_address_t)target, observed,
                          sizeof(observed)) ||
        memcmp(observed, kInputManagerGetValuePrologue7398727,
               sizeof(observed)) != 0) {
        LOG_CORE_ERROR("[Camera] Hide action polling disabled: GetInputValue signature mismatch");
        return false;
    }

    void *manager = NULL;
    if (!safe_memory_read((mach_vm_address_t)((char *)base +
            INPUT_MANAGER_SINGLETON_PTR_OFFSET_7398727), &manager,
            sizeof(manager)) || !manager) {
        LOG_CORE_ERROR("[Camera] Hide action polling disabled: InputManager unavailable");
        return false;
    }
    g_input_manager_get_value = (InputManagerGetInputValueFn)target;
    g_input_manager = manager;
    LOG_CORE_INFO("[Camera] Binding-aware Hide action polling resolved");
    return true;
}

#define MAX_CAMERA_CANDIDATES  32

typedef struct {
    uint64_t handle;
    void *game;
    void *eoc;
    float zoom_a;
    float zoom_b;
    float desired_zoom;
    float zoom_delta;
    uint32_t mode_flags;
    float previous_zoom;
    float current_zoom;
    float current_pitch;
} CameraState;

typedef struct {
    void *world;
    void *camera;
    uint16_t sneaking_component_index;
    void *exploration_definition;
    CameraFOVSettings base_fov;
    CameraOffsetSettings base_offsets;
    CameraVector3 previous_root;
    bool has_previous_root;
} AdaptiveCameraState;

static AdaptiveCameraState g_adaptive_state;

static bool read_float(void *base, size_t offset, float *out) {
    return base && out &&
        safe_memory_read((mach_vm_address_t)((char *)base + offset), out,
                         sizeof(*out));
}

static bool read_u32(void *base, size_t offset, uint32_t *out) {
    return base && out && safe_memory_read_u32(
        (mach_vm_address_t)((char *)base + offset), out);
}

static bool plausible_float(float value) {
    return isfinite(value) && fabsf(value) < 10000.0f;
}

static bool read_zoom_limits(void *definition, CameraZoomLimits *out) {
    return read_float(definition, CAMERA_DEFINITION_MIN_ZOOM_OFFSET,
                      &out->min) &&
        read_float(definition, CAMERA_DEFINITION_MAX_ZOOM_OFFSET,
                   &out->max) &&
        read_float(definition, CAMERA_DEFINITION_TACTICAL_MIN_ZOOM_OFFSET,
                   &out->tactical_min) &&
        read_float(definition, CAMERA_DEFINITION_TACTICAL_MAX_ZOOM_OFFSET,
                   &out->tactical_max) &&
        read_float(definition, CAMERA_DEFINITION_ALT_MIN_ZOOM_OFFSET,
                   &out->alt_min) &&
        read_float(definition, CAMERA_DEFINITION_ALT_MAX_ZOOM_OFFSET,
                   &out->alt_max);
}

static bool plausible_zoom_limits(const CameraZoomLimits *limits) {
    return isfinite(limits->min) && isfinite(limits->max) &&
        isfinite(limits->tactical_min) && isfinite(limits->tactical_max) &&
        isfinite(limits->alt_min) && isfinite(limits->alt_max) &&
        limits->min >= 0.05f && limits->max <= 200.0f &&
        limits->tactical_min >= 0.05f && limits->tactical_max <= 200.0f &&
        limits->alt_min >= 0.05f && limits->alt_max <= 200.0f &&
        limits->min < limits->max &&
        limits->tactical_min < limits->tactical_max &&
        limits->alt_min < limits->alt_max;
}

static bool read_fov_settings(void *definition, CameraFOVSettings *out) {
    return read_float(definition, CAMERA_DEFINITION_FOV_CLOSE_OFFSET,
                      &out->close) &&
        read_float(definition, CAMERA_DEFINITION_FOV_FAR_OFFSET,
                   &out->far) &&
        read_float(definition, CAMERA_DEFINITION_TACTICAL_FOV_OFFSET,
                   &out->tactical) &&
        read_float(definition, CAMERA_DEFINITION_ALT_FOV_CLOSE_OFFSET,
                   &out->alt_close) &&
        read_float(definition, CAMERA_DEFINITION_ALT_FOV_FAR_OFFSET,
                   &out->alt_far);
}

static bool plausible_fov_settings(const CameraFOVSettings *settings) {
    return isfinite(settings->close) && isfinite(settings->far) &&
        isfinite(settings->tactical) && isfinite(settings->alt_close) &&
        isfinite(settings->alt_far) &&
        settings->close >= 1.0f && settings->close <= 170.0f &&
        settings->far >= 1.0f && settings->far <= 170.0f &&
        settings->tactical >= 1.0f && settings->tactical <= 170.0f &&
        settings->alt_close >= 1.0f && settings->alt_close <= 170.0f &&
        settings->alt_far >= 1.0f && settings->alt_far <= 170.0f;
}

static bool read_offset_settings(void *definition,
                                 CameraOffsetSettings *out) {
    return read_float(definition, CAMERA_DEFINITION_HORIZONTAL_OFFSET,
                      &out->horizontal) &&
        read_float(definition, CAMERA_DEFINITION_VERTICAL_OFFSET,
                   &out->vertical) &&
        read_float(definition, CAMERA_DEFINITION_ALT_HORIZONTAL_OFFSET,
                   &out->alt_horizontal) &&
        read_float(definition, CAMERA_DEFINITION_ALT_VERTICAL_OFFSET,
                   &out->alt_vertical);
}

static bool plausible_offset_settings(const CameraOffsetSettings *settings) {
    return isfinite(settings->horizontal) && isfinite(settings->vertical) &&
        isfinite(settings->alt_horizontal) &&
        isfinite(settings->alt_vertical) &&
        fabsf(settings->horizontal) <= 10.0f &&
        fabsf(settings->vertical) <= 10.0f &&
        fabsf(settings->alt_horizontal) <= 10.0f &&
        fabsf(settings->alt_vertical) <= 10.0f;
}

static bool read_follow_settings(void *definition,
                                 CameraFollowSettings *out) {
    return read_float(definition,
                      CAMERA_DEFINITION_TARGET_FOLLOW_SPEED_OFFSET,
                      &out->speed);
}

static bool plausible_follow_settings(const CameraFollowSettings *settings) {
    return isfinite(settings->speed) && settings->speed >= 0.01f &&
        settings->speed <= 1000.0f;
}

static bool resolve_camera_definitions(CameraDefinitionState *out,
                                       const char **reason) {
    memset(out, 0, sizeof(*out));
    if (!version_detect_addresses_safe()) {
        *reason = "camera definitions are not verified for this game build";
        return false;
    }

    void *base = version_detect_get_binary_base();
    uint64_t singleton = 0;
    if (!base || !safe_memory_read_u64(
            (mach_vm_address_t)((char *)base +
                CAMERA_DEFINITION_SINGLETON_PTR_OFFSET_7398727),
            &singleton) || singleton == 0) {
        *reason = "camera definition singleton is not available";
        return false;
    }

    out->singleton = (void *)(uintptr_t)singleton;
    out->exploration = (char *)out->singleton +
        CAMERA_DEFINITION_EXPLORATION_OFFSET;
    out->combat = (char *)out->singleton + CAMERA_DEFINITION_COMBAT_OFFSET;
    if (!read_zoom_limits(out->exploration, &out->exploration_limits) ||
        !read_zoom_limits(out->combat, &out->combat_limits) ||
        !plausible_zoom_limits(&out->exploration_limits) ||
        !plausible_zoom_limits(&out->combat_limits)) {
        *reason = "camera definition zoom fields failed structural validation";
        return false;
    }

    /* Other capability families validate independently so an unavailable FOV
     * or offset field never disables already-proven zoom behavior. */
    read_fov_settings(out->exploration, &out->exploration_fov);
    read_fov_settings(out->combat, &out->combat_fov);
    read_offset_settings(out->exploration, &out->exploration_offsets);
    read_offset_settings(out->combat, &out->combat_offsets);
    read_follow_settings(out->exploration, &out->exploration_follow);
    read_follow_settings(out->combat, &out->combat_follow);

    *reason = NULL;
    return true;
}

static bool write_definition_float(void *definition, size_t offset,
                                   float value) {
    if (!safe_memory_write(
            (mach_vm_address_t)((char *)definition + offset), &value,
            sizeof(value))) {
        return false;
    }
    float observed = 0.0f;
    return read_float(definition, offset, &observed) &&
        fabsf(observed - value) <= 0.001f;
}

static bool write_zoom_limits(void *definition,
                              const CameraZoomLimits *limits) {
    return write_definition_float(definition,
               CAMERA_DEFINITION_MIN_ZOOM_OFFSET, limits->min) &&
        write_definition_float(definition,
               CAMERA_DEFINITION_MAX_ZOOM_OFFSET, limits->max) &&
        write_definition_float(definition,
               CAMERA_DEFINITION_TACTICAL_MIN_ZOOM_OFFSET,
               limits->tactical_min) &&
        write_definition_float(definition,
               CAMERA_DEFINITION_TACTICAL_MAX_ZOOM_OFFSET,
               limits->tactical_max) &&
        write_definition_float(definition,
               CAMERA_DEFINITION_ALT_MIN_ZOOM_OFFSET, limits->alt_min) &&
        write_definition_float(definition,
               CAMERA_DEFINITION_ALT_MAX_ZOOM_OFFSET, limits->alt_max);
}

static bool write_fov_settings(void *definition,
                               const CameraFOVSettings *settings) {
    return write_definition_float(definition,
               CAMERA_DEFINITION_FOV_CLOSE_OFFSET, settings->close) &&
        write_definition_float(definition,
               CAMERA_DEFINITION_FOV_FAR_OFFSET, settings->far) &&
        write_definition_float(definition,
               CAMERA_DEFINITION_TACTICAL_FOV_OFFSET,
               settings->tactical) &&
        write_definition_float(definition,
               CAMERA_DEFINITION_ALT_FOV_CLOSE_OFFSET,
               settings->alt_close) &&
        write_definition_float(definition,
               CAMERA_DEFINITION_ALT_FOV_FAR_OFFSET, settings->alt_far);
}

static bool write_offset_settings(void *definition,
                                  const CameraOffsetSettings *settings) {
    return write_definition_float(definition,
               CAMERA_DEFINITION_HORIZONTAL_OFFSET,
               settings->horizontal) &&
        write_definition_float(definition,
               CAMERA_DEFINITION_VERTICAL_OFFSET, settings->vertical) &&
        write_definition_float(definition,
               CAMERA_DEFINITION_ALT_HORIZONTAL_OFFSET,
               settings->alt_horizontal) &&
        write_definition_float(definition,
               CAMERA_DEFINITION_ALT_VERTICAL_OFFSET,
               settings->alt_vertical);
}

static bool write_follow_settings(void *definition,
                                  const CameraFollowSettings *settings) {
    return write_definition_float(definition,
               CAMERA_DEFINITION_TARGET_FOLLOW_SPEED_OFFSET,
               settings->speed);
}

static bool read_candidate(void *world, uint64_t handle,
                           const ComponentInfo *game_info,
                           const ComponentInfo *eoc_info,
                           CameraState *out) {
    memset(out, 0, sizeof(*out));
    out->handle = handle;
    out->game = component_lookup_by_index_in_world(
        world, handle, game_info->index, GAME_CAMERA_SIZE, false);
    out->eoc = component_lookup_by_index_in_world(
        world, handle, eoc_info->index, EOC_CAMERA_SIZE, false);
    if (!out->game || !out->eoc) return false;

    if (!read_float(out->game, CAMERA_ZOOM_A_OFFSET, &out->zoom_a) ||
        !read_float(out->game, CAMERA_ZOOM_B_OFFSET, &out->zoom_b) ||
        !read_float(out->game, CAMERA_DESIRED_ZOOM_OFFSET,
                    &out->desired_zoom) ||
        !read_float(out->game, CAMERA_ZOOM_DELTA_OFFSET, &out->zoom_delta) ||
        !read_u32(out->game, CAMERA_MODE_FLAGS_OFFSET, &out->mode_flags) ||
        !read_float(out->game, CAMERA_PREV_ZOOM_OFFSET,
                    &out->previous_zoom) ||
        !read_float(out->game, CAMERA_CURRENT_ZOOM_OFFSET,
                    &out->current_zoom) ||
        !read_float(out->game, CAMERA_CURRENT_PITCH_OFFSET,
                    &out->current_pitch)) {
        return false;
    }

    return plausible_float(out->zoom_a) && plausible_float(out->zoom_b) &&
        plausible_float(out->desired_zoom) &&
        plausible_float(out->zoom_delta) &&
        plausible_float(out->previous_zoom) &&
        plausible_float(out->current_zoom) &&
        plausible_float(out->current_pitch) &&
        fabsf(out->current_pitch) <= 360.0f;
}

static bool resolve_active_camera(CameraState *out, const char **reason) {
    if (!version_detect_matches()) {
        *reason = "camera layouts are not verified for this game build";
        return false;
    }

    if (!entity_get_world_for_context(false) && !entity_discover_client_world()) {
        *reason = "client EntityWorld is not available";
        return false;
    }

    const ComponentInfo *game_info =
        component_registry_lookup("ecl::GameCameraBehavior");
    const ComponentInfo *eoc_info =
        component_registry_lookup("ecl::EocCameraBehavior");
    if (game_info && eoc_info && game_info->index == 0 &&
        eoc_info->index == 0) {
        /* TypeIds read during early startup are zero until RegisterComponents
         * runs. Refresh once the client world is live. */
        entity_retry_typeid_discovery();
        game_info = component_registry_lookup("ecl::GameCameraBehavior");
        eoc_info = component_registry_lookup("ecl::EocCameraBehavior");
    }
    if (!game_info || !eoc_info || !game_info->discovered ||
        !eoc_info->discovered || game_info->index == COMPONENT_INDEX_UNDEFINED ||
        eoc_info->index == COMPONENT_INDEX_UNDEFINED ||
        game_info->size != GAME_CAMERA_SIZE || eoc_info->size != EOC_CAMERA_SIZE) {
        entity_retry_typeid_discovery();
        game_info = component_registry_lookup("ecl::GameCameraBehavior");
        eoc_info = component_registry_lookup("ecl::EocCameraBehavior");
    }

    if (!game_info || !eoc_info || !game_info->discovered ||
        !eoc_info->discovered || game_info->index == COMPONENT_INDEX_UNDEFINED ||
        eoc_info->index == COMPONENT_INDEX_UNDEFINED ||
        game_info->size != GAME_CAMERA_SIZE || eoc_info->size != EOC_CAMERA_SIZE) {
        *reason = "camera component TypeIds are not available";
        return false;
    }

    void *world = entity_get_world_for_context(false);
    uint64_t handles[MAX_CAMERA_CANDIDATES];
    int count = component_lookup_get_all_with_component_in_world(
        world, game_info->index, handles, MAX_CAMERA_CANDIDATES);

    int plausible = 0;
    CameraState selected;
    for (int i = 0; i < count; i++) {
        CameraState candidate;
        if (read_candidate(world, handles[i], game_info, eoc_info, &candidate)) {
            selected = candidate;
            plausible++;
        }
    }

    if (plausible == 0) {
        *reason = "no active camera entity was found";
        return false;
    }
    if (plausible != 1) {
        *reason = "more than one plausible camera entity was found";
        return false;
    }

    *out = selected;
    *reason = NULL;
    return true;
}

static bool adaptive_component_index(const char *name, uint16_t *out) {
    const ComponentInfo *info = component_registry_lookup(name);
    if (!info || !info->discovered ||
        info->index == COMPONENT_INDEX_UNDEFINED) {
        return false;
    }
    *out = info->index;
    return true;
}

static bool adaptive_read_target(const AdaptiveCameraState *state,
                                 uint64_t *target) {
    if (!state || !state->camera || !target) return false;
    uint64_t input_target = atomic_load_explicit(&g_input_target_handle,
                                                 memory_order_acquire);
    if (input_target != 0) {
        *target = input_target;
        return true;
    }
    if (!safe_memory_read(
            (mach_vm_address_t)((char *)state->camera +
                                CAMERA_TARGET_ENTITY_OFFSET),
            target, sizeof(*target)) || *target == 0) {
        return false;
    }
    return true;
}

static bool adaptive_read_exact_crouching(const AdaptiveCameraState *state) {
    uint64_t target = 0;
    if (!state || state->sneaking_component_index ==
            COMPONENT_INDEX_UNDEFINED ||
        !adaptive_read_target(state, &target)) {
        return false;
    }
    void *world = (void *)atomic_load_explicit(&g_input_entity_world,
                                               memory_order_acquire);
    if (!world) world = state->world;
    if (world && (g_get_darkness_component ||
                  resolve_darkness_getter())) {
        const void *darkness = g_get_darkness_component(world, target);
        uint8_t sneaking = 0;
        if (darkness && safe_memory_read((mach_vm_address_t)darkness,
                                         &sneaking, sizeof(sneaking))) {
            return sneaking != 0;
        }
    }
    return component_lookup_by_index_in_world(
        world, target, state->sneaking_component_index,
        IS_SNEAKING_COMPONENT_SIZE, false) != NULL;
}

static float smooth_mix(float current, float target, float dt,
                        float smooth_time) {
    if (!isfinite(smooth_time) || smooth_time <= 0.001f) return target;
    float alpha = 1.0f - expf(-3.0f * dt / smooth_time);
    float next = current + (target - current) * alpha;
    return fabsf(target - next) < 0.001f ? target : next;
}

static void apply_adaptive_camera(void *camera) {
    if (!camera || !atomic_load_explicit(&g_adaptive_enabled,
                                         memory_order_acquire)) {
        return;
    }
    bool crouch_enabled = atomic_load_explicit(
        &g_adaptive_crouch_enabled, memory_order_relaxed);
    if (crouch_enabled) poll_hide_action();
    adaptive_lock();
    if (!atomic_load_explicit(&g_adaptive_enabled, memory_order_relaxed)) {
        adaptive_unlock();
        return;
    }
    /* BG3 may replace the camera component while a save finishes loading. */
    if (camera != g_adaptive_state.camera) {
        g_adaptive_state.camera = camera;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ULL +
        (uint64_t)now.tv_nsec;
    uint64_t previous_ns = atomic_exchange_explicit(
        &g_adaptive_last_time_ns, now_ns, memory_order_relaxed);
    float dt = previous_ns > 0 && now_ns > previous_ns ?
        (float)(now_ns - previous_ns) / 1000000000.0f : 1.0f / 60.0f;
    dt = fminf(0.1f, fmaxf(0.0f, dt));

    float speed = atomic_float_load(&g_native_move_magnitude_bits);
    bool component_crouching = crouch_enabled &&
        adaptive_read_exact_crouching(&g_adaptive_state);
    /* DarknessComponent::m_IsSneaking can remain set after leaving Hide on
     * the client. Keep it visible for diagnostics, but use BG3's semantic
     * ToggleSneak action as the authoritative local camera state. */
    bool crouching = crouch_enabled && atomic_load_explicit(
        &g_adaptive_crouch_fallback, memory_order_relaxed);

    bool was_running = atomic_load_explicit(&g_adaptive_running,
                                            memory_order_relaxed);
    float threshold = atomic_float_load(
        &g_adaptive_run_speed_threshold_bits);
    /* Hysteresis avoids FOV flutter when animation speed sits on the cutoff. */
    bool running = speed >= threshold ||
        (was_running && speed >= threshold * 0.75f);
    float crouch_mix = smooth_mix(
        atomic_float_load(&g_adaptive_crouch_mix_bits),
        crouching ? 1.0f : 0.0f, dt,
        atomic_float_load(&g_adaptive_crouch_smooth_time_bits));
    float run_mix = smooth_mix(
        atomic_float_load(&g_adaptive_run_mix_bits), running ? 1.0f : 0.0f,
        dt, atomic_float_load(&g_adaptive_run_smooth_time_bits));

    atomic_float_store(&g_adaptive_speed_bits, speed);
    atomic_float_store(&g_adaptive_crouch_mix_bits, crouch_mix);
    atomic_float_store(&g_adaptive_run_mix_bits, run_mix);
    atomic_store_explicit(&g_adaptive_crouching, crouching,
                          memory_order_relaxed);
    atomic_store_explicit(&g_adaptive_component_crouching,
                          component_crouching, memory_order_relaxed);
    atomic_store_explicit(&g_adaptive_running, running,
                          memory_order_relaxed);

    float crouch_delta = atomic_float_load(&g_adaptive_crouch_delta_bits) *
        crouch_mix;
    float fov_boost = atomic_float_load(&g_adaptive_run_fov_boost_bits) *
        run_mix;
    CameraOffsetSettings offsets = g_adaptive_state.base_offsets;
    offsets.vertical += crouch_delta;
    offsets.alt_vertical += crouch_delta;
    CameraFOVSettings fov = g_adaptive_state.base_fov;
    fov.close += fov_boost;
    fov.far += fov_boost;
    fov.alt_close += fov_boost;
    fov.alt_far += fov_boost;

    /* The definitions are already build-gated and structurally validated.
     * Only write when smoothing changes the visible value enough to matter. */
    float observed = 0.0f;
    if (!read_float(g_adaptive_state.exploration_definition,
                    CAMERA_DEFINITION_VERTICAL_OFFSET, &observed) ||
        fabsf(observed - offsets.vertical) > 0.001f) {
        write_definition_float(g_adaptive_state.exploration_definition,
                               CAMERA_DEFINITION_VERTICAL_OFFSET,
                               offsets.vertical);
        write_definition_float(g_adaptive_state.exploration_definition,
                               CAMERA_DEFINITION_ALT_VERTICAL_OFFSET,
                               offsets.alt_vertical);
    }
    if (!read_float(g_adaptive_state.exploration_definition,
                    CAMERA_DEFINITION_FOV_CLOSE_OFFSET, &observed) ||
        fabsf(observed - fov.close) > 0.001f) {
        write_definition_float(g_adaptive_state.exploration_definition,
                               CAMERA_DEFINITION_FOV_CLOSE_OFFSET, fov.close);
        write_definition_float(g_adaptive_state.exploration_definition,
                               CAMERA_DEFINITION_FOV_FAR_OFFSET, fov.far);
        write_definition_float(g_adaptive_state.exploration_definition,
                               CAMERA_DEFINITION_ALT_FOV_CLOSE_OFFSET,
                               fov.alt_close);
        write_definition_float(g_adaptive_state.exploration_definition,
                               CAMERA_DEFINITION_ALT_FOV_FAR_OFFSET,
                               fov.alt_far);
    }
    adaptive_unlock();
}

static void push_state(lua_State *L, const CameraState *state) {
    char handle[32];
    snprintf(handle, sizeof(handle), "0x%016llx",
             (unsigned long long)state->handle);

    lua_newtable(L);
    lua_pushstring(L, handle);              lua_setfield(L, -2, "Entity");
    lua_pushnumber(L, state->zoom_a);       lua_setfield(L, -2, "ZoomA");
    lua_pushnumber(L, state->zoom_b);       lua_setfield(L, -2, "ZoomB");
    lua_pushnumber(L, state->desired_zoom); lua_setfield(L, -2, "DesiredZoom");
    lua_pushnumber(L, state->zoom_delta);   lua_setfield(L, -2, "ZoomDelta");
    lua_pushinteger(L, state->mode_flags);  lua_setfield(L, -2, "ModeFlags");
    lua_pushboolean(L, (state->mode_flags & CAMERA_MODE_COMBAT) != 0);
    lua_setfield(L, -2, "CombatMode");
    lua_pushnumber(L, state->previous_zoom);lua_setfield(L, -2, "PreviousZoom");
    lua_pushnumber(L, state->current_zoom); lua_setfield(L, -2, "CurrentZoom");
    lua_pushnumber(L, state->current_pitch);lua_setfield(L, -2, "PitchDegrees");
    bool override_enabled = atomic_load_explicit(&g_pitch_override_enabled,
                                                 memory_order_relaxed);
    lua_pushboolean(L, override_enabled);
    lua_setfield(L, -2, "PitchOverrideEnabled");
    bool mouse_pitch_enabled = atomic_load_explicit(&g_mouse_pitch_enabled,
                                                    memory_order_relaxed);
    lua_pushboolean(L, mouse_pitch_enabled);
    lua_setfield(L, -2, "MousePitchEnabled");
    if (override_enabled) {
        lua_pushnumber(L, pitch_override_value());
        lua_setfield(L, -2, "PitchOverride");
    }
}

static void push_zoom_limits(lua_State *L, const CameraZoomLimits *limits) {
    lua_newtable(L);
    lua_pushnumber(L, limits->min); lua_setfield(L, -2, "Min");
    lua_pushnumber(L, limits->max); lua_setfield(L, -2, "Max");
    lua_pushnumber(L, limits->tactical_min);
    lua_setfield(L, -2, "TacticalMin");
    lua_pushnumber(L, limits->tactical_max);
    lua_setfield(L, -2, "TacticalMax");
    lua_pushnumber(L, limits->alt_min); lua_setfield(L, -2, "AltMin");
    lua_pushnumber(L, limits->alt_max); lua_setfield(L, -2, "AltMax");
}

static void push_fov_settings(lua_State *L,
                              const CameraFOVSettings *settings) {
    lua_newtable(L);
    lua_pushnumber(L, settings->close); lua_setfield(L, -2, "Close");
    lua_pushnumber(L, settings->far); lua_setfield(L, -2, "Far");
    lua_pushnumber(L, settings->tactical);
    lua_setfield(L, -2, "Tactical");
    lua_pushnumber(L, settings->alt_close);
    lua_setfield(L, -2, "AltClose");
    lua_pushnumber(L, settings->alt_far);
    lua_setfield(L, -2, "AltFar");
}

static void push_offset_settings(lua_State *L,
                                 const CameraOffsetSettings *settings) {
    lua_newtable(L);
    lua_pushnumber(L, settings->horizontal);
    lua_setfield(L, -2, "Horizontal");
    lua_pushnumber(L, settings->vertical);
    lua_setfield(L, -2, "Vertical");
    lua_pushnumber(L, settings->alt_horizontal);
    lua_setfield(L, -2, "AltHorizontal");
    lua_pushnumber(L, settings->alt_vertical);
    lua_setfield(L, -2, "AltVertical");
}

static void push_follow_settings(lua_State *L,
                                 const CameraFollowSettings *settings) {
    lua_newtable(L);
    lua_pushnumber(L, settings->speed);
    lua_setfield(L, -2, "Speed");
}

static int lua_camera_get_state(lua_State *L) {
    CameraState state;
    const char *reason;
    if (!resolve_active_camera(&state, &reason)) {
        lua_pushnil(L);
        lua_pushstring(L, reason);
        return 2;
    }
    push_state(L, &state);

    CameraDefinitionState definitions;
    const char *definition_reason;
    if (resolve_camera_definitions(&definitions, &definition_reason)) {
        lua_newtable(L);
        push_zoom_limits(L, &definitions.exploration_limits);
        lua_setfield(L, -2, "Exploration");
        push_zoom_limits(L, &definitions.combat_limits);
        lua_setfield(L, -2, "Combat");
        lua_setfield(L, -2, "ZoomLimits");

        if (plausible_fov_settings(&definitions.exploration_fov) &&
            plausible_fov_settings(&definitions.combat_fov)) {
            lua_newtable(L);
            push_fov_settings(L, &definitions.exploration_fov);
            lua_setfield(L, -2, "Exploration");
            push_fov_settings(L, &definitions.combat_fov);
            lua_setfield(L, -2, "Combat");
            lua_setfield(L, -2, "FOV");
        }
        if (plausible_offset_settings(&definitions.exploration_offsets) &&
            plausible_offset_settings(&definitions.combat_offsets)) {
            lua_newtable(L);
            push_offset_settings(L, &definitions.exploration_offsets);
            lua_setfield(L, -2, "Exploration");
            push_offset_settings(L, &definitions.combat_offsets);
            lua_setfield(L, -2, "Combat");
            lua_setfield(L, -2, "Offsets");
        }
        if (plausible_follow_settings(&definitions.exploration_follow) &&
            plausible_follow_settings(&definitions.combat_follow)) {
            lua_newtable(L);
            push_follow_settings(L, &definitions.exploration_follow);
            lua_setfield(L, -2, "Exploration");
            push_follow_settings(L, &definitions.combat_follow);
            lua_setfield(L, -2, "Combat");
            lua_setfield(L, -2, "Follow");
        }
    }
    lua_pushboolean(L, g_zoom_override_enabled);
    lua_setfield(L, -2, "ZoomLimitsOverrideEnabled");
    lua_pushboolean(L, g_fov_override_enabled);
    lua_setfield(L, -2, "FOVOverrideEnabled");
    lua_pushboolean(L, g_offset_override_enabled);
    lua_setfield(L, -2, "OffsetsOverrideEnabled");
    lua_pushboolean(L, g_follow_override_enabled);
    lua_setfield(L, -2, "FollowOverrideEnabled");
    bool zoom_toggle_enabled = atomic_load_explicit(&g_zoom_toggle_enabled,
                                                    memory_order_relaxed);
    lua_pushboolean(L, zoom_toggle_enabled);
    lua_setfield(L, -2, "ZoomToggleEnabled");
    if (zoom_toggle_enabled) {
        lua_newtable(L);
        lua_pushnumber(L, atomic_float_load(&g_zoom_toggle_close_bits));
        lua_setfield(L, -2, "Close");
        lua_pushnumber(L, atomic_float_load(&g_zoom_toggle_far_bits));
        lua_setfield(L, -2, "Far");
        lua_pushnumber(L, atomic_float_load(&g_zoom_toggle_selected_bits));
        lua_setfield(L, -2, "Selected");
        lua_pushnumber(L,
            atomic_float_load(&g_zoom_toggle_smooth_time_bits));
        lua_setfield(L, -2, "SmoothTime");
        lua_setfield(L, -2, "ZoomToggle");
    }
    lua_pushboolean(L, atomic_load_explicit(&g_floor_protection_enabled,
                                            memory_order_relaxed));
    lua_setfield(L, -2, "FloorProtectionEnabled");
    bool adaptive_enabled = atomic_load_explicit(&g_adaptive_enabled,
                                                  memory_order_acquire);
    lua_pushboolean(L, adaptive_enabled);
    lua_setfield(L, -2, "AdaptiveEnabled");
    if (adaptive_enabled) {
        lua_newtable(L);
        lua_pushboolean(L, atomic_load_explicit(&g_adaptive_crouch_enabled,
                                                memory_order_relaxed));
        lua_setfield(L, -2, "CrouchEnabled");
        lua_pushboolean(L, atomic_load_explicit(&g_adaptive_crouching,
                                                memory_order_relaxed));
        lua_setfield(L, -2, "Crouching");
        lua_pushboolean(L, atomic_load_explicit(
            &g_adaptive_component_crouching, memory_order_relaxed));
        lua_setfield(L, -2, "ComponentCrouching");
        lua_pushboolean(L, atomic_load_explicit(&g_adaptive_running,
                                                memory_order_relaxed));
        lua_setfield(L, -2, "Running");
        lua_pushboolean(L, atomic_load_explicit(
            &g_adaptive_crouch_fallback, memory_order_relaxed));
        lua_setfield(L, -2, "CrouchKeyFallback");
        lua_pushnumber(L, atomic_float_load(&g_adaptive_speed_bits));
        lua_setfield(L, -2, "Speed");
        lua_pushnumber(L, atomic_float_load(&g_adaptive_crouch_mix_bits));
        lua_setfield(L, -2, "CrouchMix");
        lua_pushnumber(L, atomic_float_load(&g_adaptive_run_mix_bits));
        lua_setfield(L, -2, "RunMix");
        lua_pushboolean(L, g_input_event_hook_installed);
        lua_setfield(L, -2, "InputHookInstalled");
        lua_pushboolean(L, g_rotated_input_hook_installed);
        lua_setfield(L, -2, "MovementHookInstalled");
        lua_pushboolean(L, g_get_darkness_component != NULL);
        lua_setfield(L, -2, "TypedHideLookupAvailable");
        lua_pushboolean(L, g_game_input_hook_installed);
        lua_setfield(L, -2, "GlobalInputHookInstalled");
        lua_pushinteger(L, atomic_load_explicit(&g_game_input_last_command,
                                                memory_order_relaxed));
        lua_setfield(L, -2, "GlobalLastInputCommand");
        lua_pushboolean(L, atomic_load_explicit(&g_game_input_last_active,
                                                memory_order_relaxed));
        lua_setfield(L, -2, "GlobalLastInputActive");
        lua_pushnumber(L, atomic_float_load(&g_hide_action_value_bits));
        lua_setfield(L, -2, "HideActionValue");
        lua_pushinteger(L, atomic_load_explicit(&g_hide_action_toggle_count,
                                                memory_order_relaxed));
        lua_setfield(L, -2, "HideActionToggleCount");
        lua_pushnumber(L,
            atomic_float_load(&g_native_move_magnitude_bits));
        lua_setfield(L, -2, "NativeMoveMagnitude");
        lua_pushnumber(L, (lua_Number)atomic_load_explicit(
            &g_rotated_input_calls, memory_order_relaxed));
        lua_setfield(L, -2, "NativeMoveCalls");
        lua_pushinteger(L, atomic_load_explicit(&g_input_movement_mask,
                                                memory_order_relaxed));
        lua_setfield(L, -2, "MovementMask");
        lua_pushinteger(L, atomic_load_explicit(&g_input_last_command,
                                                memory_order_relaxed));
        lua_setfield(L, -2, "LastInputCommand");
        lua_pushboolean(L, atomic_load_explicit(&g_input_last_active,
                                                memory_order_relaxed));
        lua_setfield(L, -2, "LastInputActive");
        char input_target[32];
        snprintf(input_target, sizeof(input_target), "0x%016llx",
                 (unsigned long long)atomic_load_explicit(
                     &g_input_target_handle, memory_order_acquire));
        lua_pushstring(L, input_target);
        lua_setfield(L, -2, "InputTarget");
        char input_world[32];
        snprintf(input_world, sizeof(input_world), "0x%016llx",
                 (unsigned long long)atomic_load_explicit(
                     &g_input_entity_world, memory_order_acquire));
        lua_pushstring(L, input_world);
        lua_setfield(L, -2, "InputWorld");
        lua_setfield(L, -2, "Adaptive");
    }
    return 1;
}

static int set_game_float(lua_State *L, size_t offset, float min_value,
                          float max_value) {
    lua_Number requested = luaL_checknumber(L, 1);
    if (!isfinite((double)requested) || requested < min_value ||
        requested > max_value) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "value is outside the verified safe range");
        return 2;
    }

    CameraState state;
    const char *reason;
    if (!resolve_active_camera(&state, &reason)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, reason);
        return 2;
    }

    float value = (float)requested;
    if (!safe_memory_write(
            (mach_vm_address_t)((char *)state.game + offset), &value,
            sizeof(value))) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "camera write failed");
        return 2;
    }

    float observed = 0.0f;
    if (!read_float(state.game, offset, &observed) ||
        fabsf(observed - value) > 0.001f) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "camera did not retain the requested value");
        return 2;
    }

    lua_pushboolean(L, true);
    return 1;
}

static int lua_camera_set_pitch(lua_State *L) {
    lua_Number requested = luaL_checknumber(L, 1);
    if (!isfinite((double)requested) || requested < -89.0 ||
        requested > 89.0) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "value is outside the verified safe range");
        return 2;
    }

    CameraState state;
    const char *reason;
    if (!resolve_active_camera(&state, &reason)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, reason);
        return 2;
    }
    if (!g_pitch_hook_installed && !install_pitch_hook()) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "native pitch hook is unavailable");
        return 2;
    }

    float value = (float)requested;
    atomic_store_explicit(&g_mouse_pitch_enabled, false,
                          memory_order_release);
    atomic_store_explicit(&g_mouse_delta_y, 0, memory_order_relaxed);
    atomic_float_store(&g_pitch_override_bits, value);
    atomic_store_explicit(&g_pitch_override_enabled, true,
                          memory_order_release);

    /* Make state inspection reflect the request immediately. The native pitch
     * calculation hook returns the same value on every camera update. */
    safe_memory_write((mach_vm_address_t)((char *)state.game +
                          CAMERA_CURRENT_PITCH_OFFSET), &value, sizeof(value));

    lua_pushboolean(L, true);
    return 1;
}

static int lua_camera_clear_pitch(lua_State *L) {
    atomic_store_explicit(&g_mouse_pitch_enabled, false,
                          memory_order_release);
    atomic_store_explicit(&g_mouse_delta_y, 0, memory_order_relaxed);
    atomic_store_explicit(&g_pitch_override_enabled, false,
                          memory_order_release);
    lua_pushboolean(L, true);
    return 1;
}

static lua_Number number_field(lua_State *L, int table, const char *name,
                               lua_Number fallback) {
    lua_getfield(L, table, name);
    lua_Number value = lua_isnil(L, -1) ? fallback : luaL_checknumber(L, -1);
    lua_pop(L, 1);
    return value;
}

static bool boolean_field(lua_State *L, int table, const char *name,
                          bool fallback) {
    lua_getfield(L, table, name);
    bool value = lua_isnil(L, -1) ? fallback : lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

static void zoom_limits_from_table(lua_State *L, int table,
                                   const CameraZoomLimits *fallback,
                                   CameraZoomLimits *out) {
    out->min = (float)number_field(L, table, "Min", fallback->min);
    out->max = (float)number_field(L, table, "Max", fallback->max);
    out->tactical_min = (float)number_field(
        L, table, "TacticalMin", fallback->tactical_min);
    out->tactical_max = (float)number_field(
        L, table, "TacticalMax", fallback->tactical_max);
    out->alt_min = (float)number_field(
        L, table, "AltMin", fallback->alt_min);
    out->alt_max = (float)number_field(
        L, table, "AltMax", fallback->alt_max);
}

static void zoom_limits_named_table(lua_State *L, int options,
                                    const char *name,
                                    const CameraZoomLimits *fallback,
                                    CameraZoomLimits *out) {
    lua_getfield(L, options, name);
    if (lua_isnil(L, -1)) {
        *out = *fallback;
    } else {
        luaL_checktype(L, -1, LUA_TTABLE);
        zoom_limits_from_table(L, lua_gettop(L), fallback, out);
    }
    lua_pop(L, 1);
}

static int lua_camera_set_zoom_limits(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    CameraDefinitionState current;
    const char *reason;
    if (!resolve_camera_definitions(&current, &reason)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, reason);
        return 2;
    }

    CameraZoomLimits exploration;
    CameraZoomLimits combat;
    zoom_limits_named_table(L, 1, "Exploration",
                            &current.exploration_limits, &exploration);
    zoom_limits_named_table(L, 1, "Combat", &current.combat_limits, &combat);
    if (!plausible_zoom_limits(&exploration) ||
        !plausible_zoom_limits(&combat)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "zoom limits are outside the verified safe range");
        return 2;
    }

    if (!g_zoom_snapshot_valid ||
        g_zoom_snapshot.singleton != current.singleton) {
        g_zoom_snapshot = current;
        g_zoom_snapshot_valid = true;
    }

    if (!write_zoom_limits(current.exploration, &exploration) ||
        !write_zoom_limits(current.combat, &combat)) {
        /* Keep the two definition blocks transactional if any write fails. */
        write_zoom_limits(current.exploration, &current.exploration_limits);
        write_zoom_limits(current.combat, &current.combat_limits);
        lua_pushboolean(L, false);
        lua_pushstring(L, "camera definition zoom write failed");
        return 2;
    }

    g_zoom_override_enabled = true;
    LOG_CORE_INFO("[Camera] Zoom limits enabled: exploration %.2f..%.2f, combat %.2f..%.2f",
                  exploration.min, exploration.max, combat.min, combat.max);
    lua_pushboolean(L, true);
    return 1;
}

static int lua_camera_clear_zoom_limits(lua_State *L) {
    if (!g_zoom_snapshot_valid) {
        g_zoom_override_enabled = false;
        lua_pushboolean(L, true);
        return 1;
    }

    CameraDefinitionState current;
    const char *reason;
    if (!resolve_camera_definitions(&current, &reason)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, reason);
        return 2;
    }
    if (current.singleton != g_zoom_snapshot.singleton) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "camera definition singleton changed; refusing stale restore");
        return 2;
    }

    if (!write_zoom_limits(current.exploration,
                           &g_zoom_snapshot.exploration_limits) ||
        !write_zoom_limits(current.combat,
                           &g_zoom_snapshot.combat_limits)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "camera definition zoom restore failed");
        return 2;
    }

    g_zoom_snapshot_valid = false;
    g_zoom_override_enabled = false;
    LOG_CORE_INFO("[Camera] Vanilla zoom limits restored");
    lua_pushboolean(L, true);
    return 1;
}

static void fov_from_table(lua_State *L, int table,
                           const CameraFOVSettings *fallback,
                           CameraFOVSettings *out) {
    out->close = (float)number_field(L, table, "Close", fallback->close);
    out->far = (float)number_field(L, table, "Far", fallback->far);
    out->tactical = (float)number_field(
        L, table, "Tactical", fallback->tactical);
    out->alt_close = (float)number_field(
        L, table, "AltClose", fallback->alt_close);
    out->alt_far = (float)number_field(
        L, table, "AltFar", fallback->alt_far);
}

static void fov_named_table(lua_State *L, int options, const char *name,
                            const CameraFOVSettings *fallback,
                            CameraFOVSettings *out) {
    lua_getfield(L, options, name);
    if (lua_isnil(L, -1)) {
        *out = *fallback;
    } else {
        luaL_checktype(L, -1, LUA_TTABLE);
        fov_from_table(L, lua_gettop(L), fallback, out);
    }
    lua_pop(L, 1);
}

static int lua_camera_set_fov(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    CameraDefinitionState current;
    const char *reason;
    if (!resolve_camera_definitions(&current, &reason)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, reason);
        return 2;
    }
    if (!plausible_fov_settings(&current.exploration_fov) ||
        !plausible_fov_settings(&current.combat_fov)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "camera definition FOV fields failed structural validation");
        return 2;
    }

    CameraFOVSettings exploration;
    CameraFOVSettings combat;
    fov_named_table(L, 1, "Exploration", &current.exploration_fov,
                    &exploration);
    fov_named_table(L, 1, "Combat", &current.combat_fov, &combat);
    if (!plausible_fov_settings(&exploration) ||
        !plausible_fov_settings(&combat)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "FOV settings are outside the verified safe range");
        return 2;
    }

    if (!g_fov_snapshot_valid || g_fov_snapshot.singleton != current.singleton) {
        g_fov_snapshot = current;
        g_fov_snapshot_valid = true;
    }
    if (!write_fov_settings(current.exploration, &exploration) ||
        !write_fov_settings(current.combat, &combat)) {
        write_fov_settings(current.exploration, &current.exploration_fov);
        write_fov_settings(current.combat, &current.combat_fov);
        lua_pushboolean(L, false);
        lua_pushstring(L, "camera definition FOV write failed");
        return 2;
    }

    g_fov_override_enabled = true;
    LOG_CORE_INFO("[Camera] FOV enabled: exploration %.2f/%.2f, combat %.2f/%.2f",
                  exploration.close, exploration.far,
                  combat.close, combat.far);
    lua_pushboolean(L, true);
    return 1;
}

static int lua_camera_clear_fov(lua_State *L) {
    if (!g_fov_snapshot_valid) {
        g_fov_override_enabled = false;
        lua_pushboolean(L, true);
        return 1;
    }
    CameraDefinitionState current;
    const char *reason;
    if (!resolve_camera_definitions(&current, &reason)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, reason);
        return 2;
    }
    if (current.singleton != g_fov_snapshot.singleton) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "camera definition singleton changed; refusing stale restore");
        return 2;
    }
    if (!write_fov_settings(current.exploration,
                            &g_fov_snapshot.exploration_fov) ||
        !write_fov_settings(current.combat, &g_fov_snapshot.combat_fov)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "camera definition FOV restore failed");
        return 2;
    }
    g_fov_snapshot_valid = false;
    g_fov_override_enabled = false;
    LOG_CORE_INFO("[Camera] Vanilla FOV restored");
    lua_pushboolean(L, true);
    return 1;
}

static void offsets_from_table(lua_State *L, int table,
                               const CameraOffsetSettings *fallback,
                               CameraOffsetSettings *out) {
    out->horizontal = (float)number_field(
        L, table, "Horizontal", fallback->horizontal);
    out->vertical = (float)number_field(
        L, table, "Vertical", fallback->vertical);
    out->alt_horizontal = (float)number_field(
        L, table, "AltHorizontal", fallback->alt_horizontal);
    out->alt_vertical = (float)number_field(
        L, table, "AltVertical", fallback->alt_vertical);
}

static void offsets_named_table(lua_State *L, int options, const char *name,
                                const CameraOffsetSettings *fallback,
                                CameraOffsetSettings *out) {
    lua_getfield(L, options, name);
    if (lua_isnil(L, -1)) {
        *out = *fallback;
    } else {
        luaL_checktype(L, -1, LUA_TTABLE);
        offsets_from_table(L, lua_gettop(L), fallback, out);
    }
    lua_pop(L, 1);
}

static int lua_camera_set_offsets(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    CameraDefinitionState current;
    const char *reason;
    if (!resolve_camera_definitions(&current, &reason)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, reason);
        return 2;
    }
    if (!plausible_offset_settings(&current.exploration_offsets) ||
        !plausible_offset_settings(&current.combat_offsets)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "camera definition offset fields failed structural validation");
        return 2;
    }

    CameraOffsetSettings exploration;
    CameraOffsetSettings combat;
    offsets_named_table(L, 1, "Exploration", &current.exploration_offsets,
                        &exploration);
    offsets_named_table(L, 1, "Combat", &current.combat_offsets, &combat);
    if (!plausible_offset_settings(&exploration) ||
        !plausible_offset_settings(&combat)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "camera offsets are outside the verified safe range");
        return 2;
    }

    if (!g_offset_snapshot_valid ||
        g_offset_snapshot.singleton != current.singleton) {
        g_offset_snapshot = current;
        g_offset_snapshot_valid = true;
    }
    if (!write_offset_settings(current.exploration, &exploration) ||
        !write_offset_settings(current.combat, &combat)) {
        write_offset_settings(current.exploration,
                              &current.exploration_offsets);
        write_offset_settings(current.combat, &current.combat_offsets);
        lua_pushboolean(L, false);
        lua_pushstring(L, "camera definition offset write failed");
        return 2;
    }

    g_offset_override_enabled = true;
    LOG_CORE_INFO("[Camera] Offsets enabled: exploration %.2f/%.2f, combat %.2f/%.2f",
                  exploration.horizontal, exploration.vertical,
                  combat.horizontal, combat.vertical);
    lua_pushboolean(L, true);
    return 1;
}

static int lua_camera_clear_offsets(lua_State *L) {
    if (!g_offset_snapshot_valid) {
        g_offset_override_enabled = false;
        lua_pushboolean(L, true);
        return 1;
    }
    CameraDefinitionState current;
    const char *reason;
    if (!resolve_camera_definitions(&current, &reason)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, reason);
        return 2;
    }
    if (current.singleton != g_offset_snapshot.singleton) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "camera definition singleton changed; refusing stale restore");
        return 2;
    }
    if (!write_offset_settings(current.exploration,
                               &g_offset_snapshot.exploration_offsets) ||
        !write_offset_settings(current.combat,
                               &g_offset_snapshot.combat_offsets)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "camera definition offset restore failed");
        return 2;
    }
    g_offset_snapshot_valid = false;
    g_offset_override_enabled = false;
    LOG_CORE_INFO("[Camera] Vanilla offsets restored");
    lua_pushboolean(L, true);
    return 1;
}

static int lua_camera_set_follow_speed(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_Number multiplier = number_field(L, 1, "SpeedMultiplier", 1.0);
    bool exploration_only = boolean_field(L, 1, "ExplorationOnly", false);
    if (!isfinite((double)multiplier) || multiplier < 0.25 ||
        multiplier > 8.0) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "follow speed multiplier is outside the verified safe range");
        return 2;
    }

    CameraDefinitionState current;
    const char *reason;
    if (!resolve_camera_definitions(&current, &reason)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, reason);
        return 2;
    }
    if (!plausible_follow_settings(&current.exploration_follow) ||
        !plausible_follow_settings(&current.combat_follow)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "camera definition follow fields failed structural validation");
        return 2;
    }

    if (!g_follow_snapshot_valid ||
        g_follow_snapshot.singleton != current.singleton) {
        g_follow_snapshot = current;
        g_follow_snapshot_valid = true;
    }

    CameraFollowSettings exploration = {
        .speed = g_follow_snapshot.exploration_follow.speed *
            (float)multiplier,
    };
    CameraFollowSettings combat = {
        .speed = g_follow_snapshot.combat_follow.speed *
            (exploration_only ? 1.0f : (float)multiplier),
    };
    if (!plausible_follow_settings(&exploration) ||
        !plausible_follow_settings(&combat)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "resulting follow speeds are outside the verified safe range");
        return 2;
    }

    if (!write_follow_settings(current.exploration, &exploration) ||
        !write_follow_settings(current.combat, &combat)) {
        write_follow_settings(current.exploration, &current.exploration_follow);
        write_follow_settings(current.combat, &current.combat_follow);
        lua_pushboolean(L, false);
        lua_pushstring(L, "camera definition follow speed write failed");
        return 2;
    }

    g_follow_override_enabled = true;
    LOG_CORE_INFO("[Camera] Follow speed enabled: exploration %.2f -> %.2f, combat %.2f -> %.2f (%.2fx%s)",
                  g_follow_snapshot.exploration_follow.speed,
                  exploration.speed,
                  g_follow_snapshot.combat_follow.speed,
                  combat.speed, (double)multiplier,
                  exploration_only ? ", exploration only" : "");
    lua_pushboolean(L, true);
    return 1;
}

static int lua_camera_clear_follow_speed(lua_State *L) {
    if (!g_follow_snapshot_valid) {
        g_follow_override_enabled = false;
        lua_pushboolean(L, true);
        return 1;
    }

    CameraDefinitionState current;
    const char *reason;
    if (!resolve_camera_definitions(&current, &reason)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, reason);
        return 2;
    }
    if (current.singleton != g_follow_snapshot.singleton) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "camera definition singleton changed; refusing stale restore");
        return 2;
    }
    if (!write_follow_settings(current.exploration,
                               &g_follow_snapshot.exploration_follow) ||
        !write_follow_settings(current.combat,
                               &g_follow_snapshot.combat_follow)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "camera definition follow speed restore failed");
        return 2;
    }

    g_follow_snapshot_valid = false;
    g_follow_override_enabled = false;
    LOG_CORE_INFO("[Camera] Vanilla follow speed restored");
    lua_pushboolean(L, true);
    return 1;
}

static int lua_camera_enable_zoom_toggle(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_Number close = number_field(L, 1, "Close", 2.75);
    lua_Number far = number_field(L, 1, "Far", 6.0);
    lua_Number smooth_time = number_field(L, 1, "SmoothTime", 0.20);
    bool invert = boolean_field(L, 1, "Invert", false);
    if (!isfinite((double)close) || !isfinite((double)far) ||
        !isfinite((double)smooth_time) || close < 0.1 || far > 100.0 ||
        close >= far || smooth_time < 0.0 || smooth_time > 2.0) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "zoom toggle distances or smoothing are outside the verified safe range");
        return 2;
    }

    CameraState state;
    const char *reason;
    if (!resolve_active_camera(&state, &reason)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, reason);
        return 2;
    }
    if (!g_pitch_hook_installed && !install_pitch_hook()) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "native camera update hook is unavailable");
        return 2;
    }

    float close_value = (float)close;
    float far_value = (float)far;
    float selected = close_value;
    atomic_float_store(&g_zoom_toggle_close_bits, close_value);
    atomic_float_store(&g_zoom_toggle_far_bits, far_value);
    atomic_float_store(&g_zoom_toggle_selected_bits, selected);
    atomic_float_store(&g_zoom_toggle_smooth_time_bits, (float)smooth_time);
    atomic_store_explicit(&g_zoom_toggle_last_time_ns, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&g_zoom_toggle_invert, invert,
                          memory_order_relaxed);
    atomic_store_explicit(&g_zoom_toggle_enabled, true,
                          memory_order_release);
    apply_zoom_toggle(state.game);

    LOG_CORE_INFO("[Camera] Two-state zoom enabled: close %.2f, far %.2f, selected %.2f, smoothing %.2fs",
                  close_value, far_value, selected, (double)smooth_time);
    lua_pushboolean(L, true);
    return 1;
}

static int lua_camera_disable_zoom_toggle(lua_State *L) {
    (void)L;
    atomic_store_explicit(&g_zoom_toggle_enabled, false,
                          memory_order_release);
    atomic_store_explicit(&g_zoom_toggle_last_time_ns, 0,
                          memory_order_relaxed);
    LOG_CORE_INFO("[Camera] Two-state zoom disabled");
    lua_pushboolean(L, true);
    return 1;
}

static int lua_camera_set_zoom_target(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_Number distance = number_field(L, 1, "Distance", 2.75);
    lua_Number smooth_time = number_field(L, 1, "SmoothTime", 0.20);
    if (!isfinite((double)distance) || !isfinite((double)smooth_time) ||
        distance < 0.1 || distance > 100.0 || smooth_time < 0.0 ||
        smooth_time > 2.0) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "zoom target distance or smoothing is outside the verified safe range");
        return 2;
    }

    CameraState state;
    const char *reason;
    if (!resolve_active_camera(&state, &reason)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, reason);
        return 2;
    }
    if (!g_pitch_hook_installed && !install_pitch_hook()) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "native camera update hook is unavailable");
        return 2;
    }

    float target = (float)distance;
    atomic_float_store(&g_zoom_toggle_close_bits, target);
    atomic_float_store(&g_zoom_toggle_far_bits, target);
    atomic_float_store(&g_zoom_toggle_selected_bits, target);
    atomic_float_store(&g_zoom_toggle_smooth_time_bits, (float)smooth_time);
    atomic_store_explicit(&g_zoom_toggle_last_time_ns, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&g_zoom_toggle_enabled, true,
                          memory_order_release);
    apply_zoom_toggle(state.game);

    LOG_CORE_INFO("[Camera] Zoom target set: %.2f, smoothing %.2fs",
                  target, (double)smooth_time);
    lua_pushboolean(L, true);
    return 1;
}

static int lua_camera_enable_profile_wheel(lua_State *L) {
    (void)L;
    atomic_store_explicit(&g_profile_wheel_pending, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&g_profile_wheel_last_time_ns, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&g_profile_wheel_enabled, true,
                          memory_order_release);
    lua_pushboolean(L, true);
    return 1;
}

static int lua_camera_disable_profile_wheel(lua_State *L) {
    (void)L;
    atomic_store_explicit(&g_profile_wheel_enabled, false,
                          memory_order_release);
    atomic_store_explicit(&g_profile_wheel_pending, 0,
                          memory_order_relaxed);
    lua_pushboolean(L, true);
    return 1;
}

static int lua_camera_consume_profile_wheel(lua_State *L) {
    int direction = atomic_exchange_explicit(&g_profile_wheel_pending, 0,
                                              memory_order_acq_rel);
    lua_pushinteger(L, direction);
    return 1;
}

static int lua_camera_enable_mouse_pitch(lua_State *L) {
    lua_Number initial = 25.0;
    lua_Number min_pitch = -85.0;
    lua_Number max_pitch = 85.0;
    lua_Number sensitivity = 0.25;
    bool invert = false;

    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TTABLE);
        initial = number_field(L, 1, "Initial", initial);
        min_pitch = number_field(L, 1, "Min", min_pitch);
        max_pitch = number_field(L, 1, "Max", max_pitch);
        sensitivity = number_field(L, 1, "Sensitivity", sensitivity);
        invert = boolean_field(L, 1, "Invert", invert);
    }

    if (!isfinite((double)initial) || !isfinite((double)min_pitch) ||
        !isfinite((double)max_pitch) || !isfinite((double)sensitivity) ||
        min_pitch < -89.0 || max_pitch > 89.0 || min_pitch >= max_pitch ||
        initial < min_pitch || initial > max_pitch || sensitivity <= 0.0 ||
        sensitivity > 10.0) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "mouse pitch options are outside the verified safe range");
        return 2;
    }

    CameraState state;
    const char *reason;
    if (!resolve_active_camera(&state, &reason)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, reason);
        return 2;
    }
    if (!g_pitch_hook_installed && !install_pitch_hook()) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "native pitch hook is unavailable");
        return 2;
    }

    float initial_value = (float)initial;
    atomic_float_store(&g_mouse_pitch_min_bits, (float)min_pitch);
    atomic_float_store(&g_mouse_pitch_max_bits, (float)max_pitch);
    atomic_float_store(&g_mouse_pitch_sensitivity_bits, (float)sensitivity);
    atomic_store_explicit(&g_mouse_pitch_invert, invert, memory_order_relaxed);
    atomic_store_explicit(&g_mouse_delta_y, 0, memory_order_relaxed);
    atomic_float_store(&g_pitch_override_bits, initial_value);
    atomic_store_explicit(&g_pitch_override_enabled, true,
                          memory_order_release);
    atomic_store_explicit(&g_mouse_pitch_enabled, true,
                          memory_order_release);

    if (!camera_is_combat(state.game)) {
        safe_memory_write((mach_vm_address_t)((char *)state.game +
                              CAMERA_CURRENT_PITCH_OFFSET), &initial_value,
                          sizeof(initial_value));
    }
    lua_pushboolean(L, true);
    return 1;
}

static int lua_camera_set_caps_lock_mouse_look(lua_State *L) {
    bool enabled = lua_toboolean(L, 1);
    atomic_store_explicit(&g_caps_lock_mouse_look_enabled, enabled,
                          memory_order_release);
    lua_pushboolean(L, true);
    return 1;
}

static int lua_camera_enable_floor_protection(lua_State *L) {
    lua_Number floor_offset = 0.1;
    lua_Number min_zoom = 0.5;
    lua_Number radius = 0.25;

    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TTABLE);
        floor_offset = number_field(L, 1, "FloorOffset", floor_offset);
        min_zoom = number_field(L, 1, "MinZoom", min_zoom);
        radius = number_field(L, 1, "Radius", radius);
    }

    if (!isfinite((double)floor_offset) ||
        !isfinite((double)min_zoom) || !isfinite((double)radius) ||
        floor_offset < 0.0 || floor_offset > 5.0 ||
        min_zoom < 0.1 || min_zoom > 10.0 ||
        radius < 0.05 || radius > 5.0) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "floor protection options are outside the verified safe range");
        return 2;
    }

    CameraState state;
    const char *reason;
    if (!resolve_active_camera(&state, &reason)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, reason);
        return 2;
    }
    if ((!g_pitch_hook_installed && !install_pitch_hook()) ||
        !resolve_floor_query()) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "native floor query is unavailable");
        return 2;
    }
    if (!level_get_client_aigrid()) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "active level AiGrid is not available");
        return 2;
    }

    atomic_float_store(&g_floor_offset_bits, (float)floor_offset);
    atomic_float_store(&g_floor_min_zoom_bits, (float)min_zoom);
    atomic_float_store(&g_floor_radius_bits, (float)radius);
    atomic_store_explicit(&g_floor_protection_enabled, true,
                          memory_order_release);
    LOG_CORE_INFO("[Camera] Floor protection enabled: offset %.2f, min zoom %.2f, radius %.2f",
                  (double)floor_offset, (double)min_zoom, (double)radius);
    lua_pushboolean(L, true);
    return 1;
}

static int lua_camera_disable_floor_protection(lua_State *L) {
    (void)L;
    atomic_store_explicit(&g_floor_protection_enabled, false,
                          memory_order_release);
    LOG_CORE_INFO("[Camera] Floor protection disabled");
    lua_pushboolean(L, true);
    return 1;
}

static bool adaptive_restore_base(void) {
    if (!g_adaptive_state.exploration_definition) return true;
    bool offsets_ok = write_definition_float(
        g_adaptive_state.exploration_definition,
        CAMERA_DEFINITION_VERTICAL_OFFSET,
        g_adaptive_state.base_offsets.vertical) &&
        write_definition_float(g_adaptive_state.exploration_definition,
        CAMERA_DEFINITION_ALT_VERTICAL_OFFSET,
        g_adaptive_state.base_offsets.alt_vertical);
    bool fov_ok = write_definition_float(
        g_adaptive_state.exploration_definition,
        CAMERA_DEFINITION_FOV_CLOSE_OFFSET, g_adaptive_state.base_fov.close) &&
        write_definition_float(g_adaptive_state.exploration_definition,
        CAMERA_DEFINITION_FOV_FAR_OFFSET, g_adaptive_state.base_fov.far) &&
        write_definition_float(g_adaptive_state.exploration_definition,
        CAMERA_DEFINITION_ALT_FOV_CLOSE_OFFSET,
        g_adaptive_state.base_fov.alt_close) &&
        write_definition_float(g_adaptive_state.exploration_definition,
        CAMERA_DEFINITION_ALT_FOV_FAR_OFFSET,
        g_adaptive_state.base_fov.alt_far);
    return offsets_ok && fov_ok;
}

static int lua_camera_enable_adaptive(lua_State *L) {
    bool crouch_enabled = false;
    lua_Number crouch_delta = -0.45;
    lua_Number crouch_smooth_time = 0.18;
    lua_Number run_fov_boost = 10.0;
    lua_Number run_speed_threshold = 0.5;
    lua_Number run_smooth_time = 0.30;
    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TTABLE);
        crouch_enabled = boolean_field(L, 1, "CrouchEnabled",
                                       crouch_enabled);
        crouch_delta = number_field(L, 1, "CrouchVerticalDelta",
                                    crouch_delta);
        crouch_smooth_time = number_field(L, 1, "CrouchSmoothTime",
                                          crouch_smooth_time);
        run_fov_boost = number_field(L, 1, "RunFOVBoost", run_fov_boost);
        run_speed_threshold = number_field(L, 1, "RunSpeedThreshold",
                                           run_speed_threshold);
        run_smooth_time = number_field(L, 1, "RunSmoothTime",
                                       run_smooth_time);
    }
    if (!isfinite((double)crouch_delta) ||
        !isfinite((double)crouch_smooth_time) ||
        !isfinite((double)run_fov_boost) ||
        !isfinite((double)run_speed_threshold) ||
        !isfinite((double)run_smooth_time) ||
        crouch_delta < -5.0 || crouch_delta > 5.0 ||
        crouch_smooth_time < 0.0 || crouch_smooth_time > 2.0 ||
        run_fov_boost < 0.0 || run_fov_boost > 40.0 ||
        run_speed_threshold < 0.01 || run_speed_threshold > 100.0 ||
        run_smooth_time < 0.0 || run_smooth_time > 2.0) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "adaptive camera options are outside the verified safe range");
        return 2;
    }

    if (!g_rotated_input_hook_installed && !install_rotated_input_hook()) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "native movement-vector hook is unavailable for this build");
        return 2;
    }
    if (crouch_enabled) {
        if (!g_input_event_hook_installed && !install_input_event_hook()) {
            lua_pushboolean(L, false);
            lua_pushstring(L, "native character input hook is unavailable for this build");
            return 2;
        }
        if (!resolve_darkness_getter()) {
            lua_pushboolean(L, false);
            lua_pushstring(L, "typed Hide-state lookup is unavailable for this build");
            return 2;
        }
        if (!g_game_input_hook_installed && !install_game_input_event_hook()) {
            lua_pushboolean(L, false);
            lua_pushstring(L, "global semantic input hook is unavailable for this build");
            return 2;
        }
        if (!resolve_input_value_reader()) {
            lua_pushboolean(L, false);
            lua_pushstring(L, "binding-aware Hide action polling is unavailable for this build");
            return 2;
        }
    }

    if (atomic_exchange_explicit(&g_adaptive_enabled, false,
                                 memory_order_acq_rel)) {
        adaptive_lock();
        adaptive_restore_base();
        memset(&g_adaptive_state, 0, sizeof(g_adaptive_state));
        adaptive_unlock();
    } else {
        adaptive_lock();
        memset(&g_adaptive_state, 0, sizeof(g_adaptive_state));
        adaptive_unlock();
    }

    CameraState camera;
    const char *reason;
    if (!resolve_active_camera(&camera, &reason)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, reason);
        return 2;
    }
    CameraDefinitionState definitions;
    if (!resolve_camera_definitions(&definitions, &reason) ||
        !plausible_fov_settings(&definitions.exploration_fov) ||
        !plausible_offset_settings(&definitions.exploration_offsets)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, reason ? reason :
            "adaptive camera definition fields failed structural validation");
        return 2;
    }

    uint16_t sneaking_index = COMPONENT_INDEX_UNDEFINED;
    if (!adaptive_component_index("eoc::sneak::IsSneakingComponent",
                                  &sneaking_index)) {
        entity_retry_typeid_discovery();
        adaptive_component_index("eoc::sneak::IsSneakingComponent",
                                 &sneaking_index);
    }

    AdaptiveCameraState next_state = {
        .world = entity_get_world_for_context(false),
        .camera = camera.game,
        .sneaking_component_index = sneaking_index,
        .exploration_definition = definitions.exploration,
        .base_fov = definitions.exploration_fov,
        .base_offsets = definitions.exploration_offsets,
    };

    if (definitions.exploration_fov.close + run_fov_boost > 170.0 ||
        definitions.exploration_fov.far + run_fov_boost > 170.0 ||
        fabs(definitions.exploration_offsets.vertical + crouch_delta) > 10.0) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "adaptive result exceeds verified camera field ranges");
        return 2;
    }

    adaptive_lock();
    g_adaptive_state = next_state;
    atomic_float_store(&g_adaptive_crouch_delta_bits, (float)crouch_delta);
    atomic_store_explicit(&g_adaptive_crouch_enabled, crouch_enabled,
                          memory_order_relaxed);
    atomic_float_store(&g_adaptive_crouch_smooth_time_bits,
                       (float)crouch_smooth_time);
    atomic_float_store(&g_adaptive_run_fov_boost_bits,
                       (float)run_fov_boost);
    atomic_float_store(&g_adaptive_run_speed_threshold_bits,
                       (float)run_speed_threshold);
    atomic_float_store(&g_adaptive_run_smooth_time_bits,
                       (float)run_smooth_time);
    atomic_float_store(&g_adaptive_speed_bits, 0.0f);
    atomic_float_store(&g_adaptive_crouch_mix_bits, 0.0f);
    atomic_float_store(&g_adaptive_run_mix_bits, 0.0f);
    atomic_store_explicit(&g_adaptive_crouching, false,
                          memory_order_relaxed);
    atomic_store_explicit(&g_adaptive_component_crouching, false,
                          memory_order_relaxed);
    atomic_store_explicit(&g_adaptive_crouch_fallback, false,
                          memory_order_relaxed);
    atomic_store_explicit(&g_adaptive_running, false,
                          memory_order_relaxed);
    atomic_store_explicit(&g_adaptive_last_time_ns, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&g_input_movement_mask, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&g_input_sneak_down, false,
                          memory_order_relaxed);
    atomic_float_store(&g_native_move_magnitude_bits, 0.0f);
    atomic_float_store(&g_hide_action_value_bits, 0.0f);
    atomic_store_explicit(&g_hide_action_toggle_count, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&g_adaptive_enabled, true, memory_order_release);
    adaptive_unlock();
    LOG_CORE_INFO("[Camera] Adaptive framing enabled: crouch %s, delta %.2f, run FOV +%.2f, speed threshold %.2f",
                  crouch_enabled ? "on" : "off", (double)crouch_delta,
                  (double)run_fov_boost,
                  (double)run_speed_threshold);
    lua_pushboolean(L, true);
    return 1;
}

static int lua_camera_disable_adaptive(lua_State *L) {
    (void)L;
    bool was_enabled = atomic_exchange_explicit(&g_adaptive_enabled, false,
                                                memory_order_acq_rel);
    adaptive_lock();
    bool restored = !was_enabled || adaptive_restore_base();
    memset(&g_adaptive_state, 0, sizeof(g_adaptive_state));
    atomic_store_explicit(&g_adaptive_last_time_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&g_adaptive_crouch_fallback, false,
                          memory_order_relaxed);
    adaptive_unlock();
    if (!restored) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "adaptive camera baseline restore failed");
        return 2;
    }
    LOG_CORE_INFO("[Camera] Adaptive framing disabled");
    lua_pushboolean(L, true);
    return 1;
}

static int lua_camera_set_zoom(lua_State *L) {
    return set_game_float(L, CAMERA_CURRENT_ZOOM_OFFSET, 0.1f, 100.0f);
}

static int lua_camera_set_distance(lua_State *L) {
    return set_game_float(L, CAMERA_ZOOM_B_OFFSET, 0.1f, 100.0f);
}

static int lua_camera_get_capabilities(lua_State *L) {
    if (!g_pitch_hook_installed) install_pitch_hook();
    resolve_floor_query();
    CameraState state;
    const char *reason;
    bool available = resolve_active_camera(&state, &reason);

    lua_newtable(L);
    lua_pushboolean(L, available); lua_setfield(L, -2, "Available");
    lua_pushboolean(L, available && g_pitch_hook_installed);
    lua_setfield(L, -2, "Pitch");
    lua_pushboolean(L, available && g_pitch_hook_installed);
    lua_setfield(L, -2, "MousePitch");
    lua_pushboolean(L, available && g_pitch_hook_installed);
    lua_setfield(L, -2, "ZoomToggle");
    CameraDefinitionState definitions;
    const char *definition_reason;
    bool zoom_limits = resolve_camera_definitions(&definitions,
                                                  &definition_reason);
    lua_pushboolean(L, zoom_limits); lua_setfield(L, -2, "ZoomLimits");
    lua_pushboolean(L, zoom_limits &&
        plausible_fov_settings(&definitions.exploration_fov) &&
        plausible_fov_settings(&definitions.combat_fov));
    lua_setfield(L, -2, "FOV");
    lua_pushboolean(L, zoom_limits &&
        plausible_offset_settings(&definitions.exploration_offsets) &&
        plausible_offset_settings(&definitions.combat_offsets));
    lua_setfield(L, -2, "Offsets");
    lua_pushboolean(L, zoom_limits &&
        plausible_follow_settings(&definitions.exploration_follow) &&
        plausible_follow_settings(&definitions.combat_follow));
    lua_setfield(L, -2, "FollowSpeed");
    lua_pushboolean(L, available && g_pitch_hook_installed &&
        g_aigrid_get_height != NULL);
    lua_setfield(L, -2, "FloorProtection");
    lua_pushboolean(L, available && g_pitch_hook_installed && zoom_limits &&
        g_input_event_hook_installed && g_rotated_input_hook_installed &&
        g_game_input_hook_installed && g_input_manager_get_value != NULL);
    lua_setfield(L, -2, "Adaptive");
    lua_pushboolean(L, g_input_event_hook_installed);
    lua_setfield(L, -2, "NativeInputHookInstalled");
    lua_pushboolean(L, g_rotated_input_hook_installed);
    lua_setfield(L, -2, "NativeMovementHookInstalled");
    lua_pushboolean(L, g_get_darkness_component != NULL);
    lua_setfield(L, -2, "TypedHideLookupAvailable");
    lua_pushboolean(L, g_game_input_hook_installed);
    lua_setfield(L, -2, "GlobalInputHookInstalled");
    lua_pushboolean(L, g_input_manager_get_value != NULL);
    lua_setfield(L, -2, "HideActionPollingAvailable");
    lua_pushboolean(L, false); lua_setfield(L, -2, "Zoom");
    lua_pushboolean(L, false); lua_setfield(L, -2, "Distance");
    lua_pushboolean(L, g_pitch_hook_installed);
    lua_setfield(L, -2, "PitchHookInstalled");
    lua_pushstring(L, version_detect_get_version() ?
        version_detect_get_version() : "unknown");
    lua_setfield(L, -2, "Build");
    if (!available && reason) {
        lua_pushstring(L, reason);
        lua_setfield(L, -2, "Reason");
    }
    return 1;
}

static const struct luaL_Reg camera_functions[] = {
    { "GetCapabilities", lua_camera_get_capabilities },
    { "GetState", lua_camera_get_state },
    { "SetPitch", lua_camera_set_pitch },
    { "ClearPitch", lua_camera_clear_pitch },
    { "EnableMousePitch", lua_camera_enable_mouse_pitch },
    { "SetCapsLockMouseLook", lua_camera_set_caps_lock_mouse_look },
    { "EnableZoomToggle", lua_camera_enable_zoom_toggle },
    { "DisableZoomToggle", lua_camera_disable_zoom_toggle },
    { "SetZoomTarget", lua_camera_set_zoom_target },
    { "EnableProfileWheel", lua_camera_enable_profile_wheel },
    { "DisableProfileWheel", lua_camera_disable_profile_wheel },
    { "ConsumeProfileWheel", lua_camera_consume_profile_wheel },
    { "EnableFloorProtection", lua_camera_enable_floor_protection },
    { "DisableFloorProtection", lua_camera_disable_floor_protection },
    { "EnableAdaptive", lua_camera_enable_adaptive },
    { "DisableAdaptive", lua_camera_disable_adaptive },
    { "SetZoomLimits", lua_camera_set_zoom_limits },
    { "ClearZoomLimits", lua_camera_clear_zoom_limits },
    { "SetFOV", lua_camera_set_fov },
    { "ClearFOV", lua_camera_clear_fov },
    { "SetOffsets", lua_camera_set_offsets },
    { "ClearOffsets", lua_camera_clear_offsets },
    { "SetFollowSpeed", lua_camera_set_follow_speed },
    { "ClearFollowSpeed", lua_camera_clear_follow_speed },
    { "SetZoom", lua_camera_set_zoom },
    { "SetDistance", lua_camera_set_distance },
    { NULL, NULL }
};

void lua_camera_register(lua_State *L, int ext_table_idx) {
    install_pitch_hook();
    install_input_event_hook();
    install_rotated_input_hook();
    resolve_darkness_getter();
    install_game_input_event_hook();
    install_app_input_event_hook();
    resolve_input_value_reader();
    int ext = lua_absindex(L, ext_table_idx);
    lua_newtable(L);
    for (const struct luaL_Reg *fn = camera_functions; fn->name; fn++) {
        lua_pushcfunction(L, fn->func);
        lua_setfield(L, -2, fn->name);
    }
    lua_setfield(L, ext, "Camera");
}
