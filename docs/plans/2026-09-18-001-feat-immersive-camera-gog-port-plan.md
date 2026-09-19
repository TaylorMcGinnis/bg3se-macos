# Porting BG3 Player Immersive Camera to the GOG build

Target: add `Ext.Camera`, `Ext.Movement` and the native-panel `Ext.UI` calls to
`bg3se-macos` on the `gog-support` branch, so the mod installs as plain data.

Source: `ldomaradzki/bg3se-macos` @ `compat/bg3-7398727` (head `ee08fef`,
2026-08-14), the fork behind `ldomaradzki/bg3-player-immersive-camera` v1.0.0.

---

## 1. Why this is tractable

`camera_system.c` contains **zero hardcoded addresses inline**. It drives the
camera through the ECS layer already ported for GOG — `component_registry_lookup()`,
`entity_get_world_for_context()`, `component_lookup_by_index_in_world()` — the
same layer whose 2004 TypeIds were recovered in `70d9d44`.

The entire store-specific surface is **12 constants**, not the ~4,700 that the
component tables carry.

Every non-camera API the mod calls already exists on `gog-support`:

| Mod call | Already provided by |
|---|---|
| `Ext.Timer.WaitFor` / `Cancel` | `src/lua/lua_timer.c` |
| `Ext.IO.LoadFile` / `SaveFile` | `src/lua/lua_ext.c` |
| `Ext.Input.IsKeyPressed` | `src/input/lua_input.c` |
| `Ext.Events.SessionLoaded` / `GameStateChanged` | `src/lua/lua_events.c` |
| `Ext.Json.Parse` / `Stringify` | `src/lua/lua_json.c` |
| `Ext.Entity.Discover` | entity layer |

So only **Camera**, **Movement** and the native-panel **UI** calls are new.

---

## 2. Resolved GOG addresses

All twelve resolved against `Contents/MacOS/"Baldur's Gate 3 GOG"` (arm64 slice,
LC_UUID `895FBE6B-5EC6-30F5-A80D-4E1F58D3DDAC`, matching
`src/gen/gog/build_identity.h`).

The binary is **not stripped** — 766,391 symbols. Every function was resolved by
mangled symbol name, then confirmed byte-for-byte against the 16-byte prologue
that `camera_system.c` already checks before hooking.

| Constant | Steam | GOG | Δ | Resolved by |
|---|---|---|---|---|
| `CAMERA_GET_PITCH` | `0x34f303c` | `0x34eaf64` | −0x80d8 | `ecl::GameCameraBehavior::GetCameraPitchDegrees(bool,bool) const` |
| `AIGRID_GET_HEIGHT` | `0x1136a94` | `0x112ea34` | −0x8060 | `eoc::AiGrid::GetHeightInArea(Vector3f const&,float) const` |
| `INPUT_CONTROLLER_ON_EVENT` | `0x2fdf38c` | `0x2fd711c` | −0x8270 | `ecl::InputController::OnInputEvent(ls::InputEvent const&)` |
| `GET_ROTATED_INPUT` | `0x3446808` | `0x343e730` | −0x80d8 | `(anonymous namespace)::GetRotatedInput(short,bool)` |
| `GET_DARKNESS_COMPONENT` | `0x157201c` | `0x1569fbc` | −0x8060 | `ecs::EntityWorld::GetComponent<eoc::DarknessComponent const,true>` |
| `GAME_INPUT_ON_EVENT` | `0x2fa0110` | `0x2f97ef0` | −0x8220 | `ecl::GameInput::OnInputEvent(ls::InputEvent const&)` |
| `APP_ON_INPUT_EVENT` | `0xc65184` | `0xc5d124` | −0x8060 | `App::OnInputEvent(ls::InputEvent const&)` |
| `INPUT_MANAGER_GET_VALUE` | `0x64d6ba8` | `0x64cdda0` | −0x8e08 | `ls::InputManager::GetInputValue(unsigned int const&,ls::EInputPlayerIndex) const` |
| `INPUT_MANAGER_SINGLETON_PTR` | `0x8b25d08` | `0x8b1e458` | −0x78b0 | `ls::InputManager::m_ptr` (__DATA) |
| `CAMERA_DEFINITION_SINGLETON_PTR` | `0x8b25f40` | `0x8b1e690` | −0x78b0 | `_Global` (__DATA), confirmed by adrp/ldr at `GetCameraPitchDegrees+0x20` |
| `MOVEMENT_UNLOCK_BRANCH` | `0x3444548` | `0x343c470` | −0x80d8 | unique `0x340016e8` in `CharacterTask_MoveController::CanExecute()` |
| `CAMERA_SHOULD_MOVE_STORE` | `0x332c0d0` | `0x3323ff8` | −0x80d8 | unique `0x390512db` in `ecl::CameraSystem::OnInputEvent(...)` |

Six distinct deltas (−0x8060, −0x80d8, −0x8220, −0x8270, −0x8e08, −0x78b0).
**No uniform shift applies** — this independently reconfirms `70d9d44`.

Two traps worth recording:

- `GET_DARKNESS_COMPONENT` has a **non-const twin** at `0x156bd08` sharing the
  identical generic frame prologue. Hooking it would pass the signature check
  and still be wrong. Only the symbol name distinguishes them.
- `APP_ON_INPUT_EVENT` and `GET_DARKNESS_COMPONENT` share the prologue
  `ff4301d1 f65702a9 f44f03a9 fd7b04a9`, which occurs **3,851 times** in
  `__TEXT`. Signature scanning alone cannot resolve either.

Landed as `src/gen/gog/camera_addresses.h` and `src/gen/steam/camera_addresses.h`.

---

## 3. Work plan

**Phase 1 — lift the constants out of the code.**
Move the 12 `#define`s out of `camera_system.c` / `movement_system.c` into
`src/gen/<store>/camera_addresses.h`. CMake already selects one `src/gen/<store>/`
via `-DBG3_STORE`; this rides that mechanism with no new machinery.

**Phase 2 — port the subsystems.** Clean additions, no existing files touched:

| From the fork | Lines |
|---|---|
| `src/camera/camera_system.{c,h}` | 2,959 |
| `src/movement/movement_system.{c,h}` | 262 |

**Phase 3 — merge the shared input layer.** `src/input/` exists on both sides
and has barely diverged:

| File | Mine | Fork | Differing lines |
|---|---|---|---|
| `focusless_input.m` / `.h` | 304 / 26 | 304 / 26 | **0 (identical)** |
| `lua_input.c` | 416 | 419 | 3 |
| `input.h` | 288 | 294 | 6 |
| `input_hooks.m` | 555 | 584 | 73 |

Only `input_hooks.m` needs real attention. `src/imgui/imgui_input_hooks.mm`
(311 lines) backs the native settings panel and needs the same treatment.

**Phase 4 — build and verify.**
`cmake -B build-gog -DBG3_STORE=gog && cmake --build build-gog`, launch via
`scripts/bg3g.sh`, and confirm in `/tmp/bg3g_debug.log` that no hook reports
`signature mismatch`.

**Phase 5 — install the mod as data** (see §5).

---

## 4. Open risks

**Input event IDs — the one genuine unknown.** `camera_system.c` hardcodes
`INPUT_CHARACTER_MOVE_FORWARD 157`, `..._BACKWARD 158`, `..._LEFT 159`,
`..._RIGHT 160`, `INPUT_TOGGLE_PRESENTATION 162`, `INPUT_TOGGLE_SNEAK 205`,
commented as "registered by this exact game build". These are assigned at
**runtime**, so they cannot be verified statically the way the addresses were.
Both stores ship identical game data, so they very probably match — but this is
the one item that must be checked by logging
`InputManager::GetInputValue` IDs on a live GOG session. If they differ, W/A/S/D
detection silently misfires while every hook still reports healthy.

**Struct field offsets are unverified.** `CAMERA_ZOOM_A_OFFSET 0x58` through
`CAMERA_CURRENT_PITCH_OFFSET 0x164`, and the definition-block offsets
(`0x7c4` exploration, `0x958` combat), are in-memory layouts. Same source and
compiler means they almost certainly hold, but nothing above proves it. Spot-check
zoom and FOV at runtime before trusting the profile system.

**The two patch sites are the sharpest edge.** They write instructions
*mid-function*, so a function-start signature cannot validate them. Both original
words verified at the resolved GOG addresses, and `CAMERA_SHOULD_MOVE_STORE`'s
word is unique across all 138MB of `__TEXT`. Keep the existing
expected-original-word check as a hard gate — never patch on address alone.

**Maintenance.** Any GOG patch invalidates all 12 constants *and* the 2004
TypeIds. Because the binary ships unstripped, re-resolving is mechanical: look
up the symbol names in §2 with `nm`, then re-verify prologues. Worth scripting as
`tools/gen_camera_addresses.py` alongside the existing generators.

**Licensing.** The fork is GPL-3.0-or-later with additional terms in its
`EXCEPTIONS` file. Read that before merging its code into your tree.

**Fail-closed behavior is preserved throughout.** Every hook verifies its
prologue and logs `signature mismatch` and disables itself rather than patching.
A wrong constant costs a feature, not memory safety — which is what makes
iterating on this cheap.

*Not a risk:* the mod's `Config.json` asks only `"RequiredVersion": 1`, and
the x86_64 slice is irrelevant since `bg3g.sh` forces `arch -arm64`.

---

## 5. Installing the mod itself (independent of the port)

The mod is pure data — no coupling to the dylib beyond the `Ext` API surface.
**Do not run `Install.command`.** On a GOG install it patches the 200KB
arch-selector stub instead of the game, and overwrites `libbg3se.dylib` with a
Steam-only build.

Instead, copy the four files and merge the bindings:

```bash
SRC=".../BG3-Player-Immersive-Camera-v1.0.0-macOS"
DATA="$HOME/Documents/Larian Studios/Baldur's Gate 3"
cp -R "$SRC/Payload/BG3PlayerImmersiveCamera" "$DATA/Mods/"
cp "$DATA/PlayerProfiles/Public/inputconfig_p1.json" ~/Desktop/inputconfig_p1.backup.json
osascript -l JavaScript "$SRC/merge-input.js" \
  "$DATA/PlayerProfiles/Public/inputconfig_p1.json"
```

Launch through `scripts/bg3g.sh` as usual. Until the port lands, the mod loads
and then errors on the first `Ext.Camera` call — harmless, but inert.
