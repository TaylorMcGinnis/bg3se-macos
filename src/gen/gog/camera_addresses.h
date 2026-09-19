/**
 * camera_addresses.h - camera/movement/input hook targets for one game build
 *
 * Proposed for src/gen/gog/. CMake puts exactly one store's copy on the
 * include path (-DBG3_STORE=steam|gog), as it already does for
 * build_identity.h and generated_typeids.h.
 *
 * Resolved against Contents/MacOS/"Baldur's Gate 3 GOG", arm64 slice,
 * LC_UUID 895FBE6B-5EC6-30F5-A80D-4E1F58D3DDAC (matches
 * src/gen/gog/build_identity.h). The binary is NOT stripped -- 766,391
 * symbols -- so every function below was resolved by mangled symbol name and
 * then independently confirmed byte-for-byte against the 16-byte prologue
 * signature that camera_system.c already verifies at hook time.
 *
 * Deltas from the Steam build are NOT uniform: -0x8060, -0x80d8, -0x8220,
 * -0x8270 and -0x8e08 in __TEXT, -0x78b0 in __DATA. Do not extrapolate.
 */

#ifndef BG3SE_GEN_CAMERA_ADDRESSES_H
#define BG3SE_GEN_CAMERA_ADDRESSES_H

/* --- __TEXT: hooked functions (prologue-verified before DobbyHook) ------- */

/* ecl::GameCameraBehavior::GetCameraPitchDegrees(bool, bool) const */
#define CAMERA_GET_PITCH_OFFSET_7398727           0x034eaf64ULL

/* eoc::AiGrid::GetHeightInArea(Vector3f const&, float) const */
#define AIGRID_GET_HEIGHT_OFFSET_7398727          0x0112ea34ULL

/* ecl::InputController::OnInputEvent(ls::InputEvent const&) */
#define INPUT_CONTROLLER_ON_EVENT_OFFSET_7398727  0x02fd711cULL

/* (anonymous namespace)::GetRotatedInput(short, bool) */
#define GET_ROTATED_INPUT_OFFSET_7398727          0x0343e730ULL

/* ecs::EntityWorld::GetComponent<eoc::DarknessComponent const, true>(...)
 * NOTE: the const-qualified instantiation. The non-const one sits at
 * 0x0156bd08 and shares the same generic frame prologue -- picking it would
 * pass the signature check and hook the wrong function. */
#define GET_DARKNESS_COMPONENT_OFFSET_7398727     0x01569fbcULL

/* ecl::GameInput::OnInputEvent(ls::InputEvent const&) */
#define GAME_INPUT_ON_EVENT_OFFSET_7398727        0x02f97ef0ULL

/* App::OnInputEvent(ls::InputEvent const&) */
#define APP_ON_INPUT_EVENT_OFFSET_7398727         0x00c5d124ULL

/* ls::InputManager::GetInputValue(unsigned int const&, ls::EInputPlayerIndex) const */
#define INPUT_MANAGER_GET_VALUE_OFFSET_7398727    0x064cdda0ULL

/* --- __DATA: singleton pointers ----------------------------------------- */

/* ls::InputManager::m_ptr */
#define INPUT_MANAGER_SINGLETON_PTR_OFFSET_7398727     0x08b1e458ULL

/* Camera configuration singleton. The symbol is named plain "_Global", so it
 * was confirmed structurally instead: GetCameraPitchDegrees reaches it at
 * +0x20 via adrp/ldr x8,[x8,#0x690]. */
#define CAMERA_DEFINITION_SINGLETON_PTR_OFFSET_7398727 0x08b1e690ULL

/* --- __TEXT: inline patch sites (instruction word verified before write) -- */

/* Inside ecl::CharacterTask_MoveController::CanExecute(), +0x158 into the
 * function. Original word 0x340016e8 occurs exactly once in that function. */
#define MOVEMENT_UNLOCK_BRANCH_OFFSET_7398727     0x0343c470ULL

/* Inside ecl::CameraSystem::OnInputEvent(ecs::EntityRef const&,
 * ls::InputEvent const&). Original word 0x390512db occurs exactly once in the
 * whole 138MB __TEXT segment, so this site is unambiguous. */
#define CAMERA_SHOULD_MOVE_STORE_OFFSET_7398727   0x03323ff8ULL

/* _gCore+0x58: selects which character movement task may run.
 * MoveController::CanExecute rejects when it is 0; MoveInDirection::CanExecute
 * rejects when it is non-zero. Diagnostic only. */
#define CORE_INPUT_MODE_FLAG_OFFSET_7398727       0x08b1e4c0ULL

/* ecl::InputController::IsInSelectorMode(...). CanExecute rejects when this
 * returns true; probed read-only to identify the gate. */
#define INPUT_IS_IN_SELECTOR_MODE_OFFSET_7398727  0x02fd9d88ULL

#endif /* BG3SE_GEN_CAMERA_ADDRESSES_H */
