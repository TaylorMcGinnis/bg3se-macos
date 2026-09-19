/**
 * camera_addresses.h - camera/movement/input hook targets for one game build
 *
 * Proposed for src/gen/steam/. Values lifted verbatim from ldomaradzki's
 * compat/bg3-7398727 branch (src/camera/camera_system.c and
 * src/movement/movement_system.c), where they were inline #defines.
 */

#ifndef BG3SE_GEN_CAMERA_ADDRESSES_H
#define BG3SE_GEN_CAMERA_ADDRESSES_H

/* --- __TEXT: hooked functions ------------------------------------------- */
#define CAMERA_GET_PITCH_OFFSET_7398727           0x034f303cULL
#define AIGRID_GET_HEIGHT_OFFSET_7398727          0x01136a94ULL
#define INPUT_CONTROLLER_ON_EVENT_OFFSET_7398727  0x02fdf38cULL
#define GET_ROTATED_INPUT_OFFSET_7398727          0x03446808ULL
#define GET_DARKNESS_COMPONENT_OFFSET_7398727     0x0157201cULL
#define GAME_INPUT_ON_EVENT_OFFSET_7398727        0x02fa0110ULL
#define APP_ON_INPUT_EVENT_OFFSET_7398727         0x00c65184ULL
#define INPUT_MANAGER_GET_VALUE_OFFSET_7398727    0x064d6ba8ULL

/* --- __DATA: singleton pointers ----------------------------------------- */
#define INPUT_MANAGER_SINGLETON_PTR_OFFSET_7398727     0x08b25d08ULL
#define CAMERA_DEFINITION_SINGLETON_PTR_OFFSET_7398727 0x08b25f40ULL

/* --- __TEXT: inline patch sites ----------------------------------------- */
#define MOVEMENT_UNLOCK_BRANCH_OFFSET_7398727     0x03444548ULL
#define CAMERA_SHOULD_MOVE_STORE_OFFSET_7398727   0x0332c0d0ULL

#endif /* BG3SE_GEN_CAMERA_ADDRESSES_H */
