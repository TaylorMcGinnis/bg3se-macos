#!/usr/bin/env python3
"""Generate src/gen/camera_addresses.{h,c} from a table of per-store addresses.

These are the camera and movement hook targets. One dylib carries a table per
supported build and picks one from the store detected at runtime, the same way
offset_table.c picks its row.

The macros are kept so callers read unchanged; each expands to a lookup rather
than a constant, so none of them can be used where a compile-time constant is
required.

Resolving these for a new build
-------------------------------
The game binary is not stripped, so resolve by mangled symbol name and then
confirm the bytes. Do not derive one store's addresses from another by delta:
the measured deltas are not uniform (-0x8060, -0x80d8, -0x8220, -0x8270,
-0x8e08 in __TEXT, -0x78b0 in __DATA).

    nm -arch arm64 "<game binary>" | c++filt | grep '<symbol>'

For the two patch sites, find the expected original instruction word within the
containing function rather than trusting an offset.

Usage:
    python3 tools/gen_camera_addresses.py            # rewrite in place
    python3 tools/gen_camera_addresses.py --check    # fail if out of date (CI)
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# field, macro, what it is
FIELDS = [
    ("camera_get_pitch", "CAMERA_GET_PITCH_OFFSET_7398727",
     "ecl::GameCameraBehavior::GetCameraPitchDegrees(bool, bool) const"),
    ("aigrid_get_height", "AIGRID_GET_HEIGHT_OFFSET_7398727",
     "eoc::AiGrid::GetHeightInArea(Vector3f const&, float) const"),
    ("input_controller_on_event", "INPUT_CONTROLLER_ON_EVENT_OFFSET_7398727",
     "ecl::InputController::OnInputEvent(ls::InputEvent const&)"),
    ("get_rotated_input", "GET_ROTATED_INPUT_OFFSET_7398727",
     "(anonymous namespace)::GetRotatedInput(short, bool)"),
    ("get_darkness_component", "GET_DARKNESS_COMPONENT_OFFSET_7398727",
     "ecs::EntityWorld::GetComponent<eoc::DarknessComponent const, true> "
     "-- the CONST instantiation; the non-const twin shares its prologue"),
    ("game_input_on_event", "GAME_INPUT_ON_EVENT_OFFSET_7398727",
     "ecl::GameInput::OnInputEvent(ls::InputEvent const&)"),
    ("app_on_input_event", "APP_ON_INPUT_EVENT_OFFSET_7398727",
     "App::OnInputEvent(ls::InputEvent const&)"),
    ("input_manager_get_value", "INPUT_MANAGER_GET_VALUE_OFFSET_7398727",
     "ls::InputManager::GetInputValue(unsigned int const&, ls::EInputPlayerIndex) const"),
    ("input_manager_singleton_ptr", "INPUT_MANAGER_SINGLETON_PTR_OFFSET_7398727",
     "ls::InputManager::m_ptr (__DATA)"),
    ("camera_definition_singleton_ptr", "CAMERA_DEFINITION_SINGLETON_PTR_OFFSET_7398727",
     "EoCGlobalSwitches (__DATA). Symbol is a bare _Global; confirm it by the "
     "adrp/ldr pair at GetCameraPitchDegrees+0x20"),
    ("movement_unlock_branch", "MOVEMENT_UNLOCK_BRANCH_OFFSET_7398727",
     "patch site in ecl::CharacterTask_MoveController::CanExecute(); "
     "find the unique word 0x340016e8 inside that function"),
    ("camera_should_move_store", "CAMERA_SHOULD_MOVE_STORE_OFFSET_7398727",
     "patch site in ecl::CameraSystem::OnInputEvent(); the word 0x390512db "
     "occurs once in the whole __TEXT segment"),
    ("core_input_mode_flag", "CORE_INPUT_MODE_FLAG_OFFSET_7398727",
     "_gCore+0x58 (__DATA). Selects which movement task may run: "
     "MoveController rejects when 0, MoveInDirection rejects when non-zero"),
]

# store -> {field: offset}. 0 means "not resolved for this build"; callers must
# treat 0 as unavailable rather than as an address.
ADDRESSES = {
    "steam": {
        "camera_get_pitch": 0x034f303c,
        "aigrid_get_height": 0x01136a94,
        "input_controller_on_event": 0x02fdf38c,
        "get_rotated_input": 0x03446808,
        "get_darkness_component": 0x0157201c,
        "game_input_on_event": 0x02fa0110,
        "app_on_input_event": 0x00c65184,
        "input_manager_get_value": 0x064d6ba8,
        "input_manager_singleton_ptr": 0x08b25d08,
        "camera_definition_singleton_ptr": 0x08b25f40,
        "movement_unlock_branch": 0x03444548,
        "camera_should_move_store": 0x0332c0d0,
        # Unresolved: no Steam binary was available. 0 disables the reading
        # rather than guessing an address by delta.
        "core_input_mode_flag": 0,
    },
    "gog": {
        "camera_get_pitch": 0x034eaf64,
        "aigrid_get_height": 0x0112ea34,
        "input_controller_on_event": 0x02fd711c,
        "get_rotated_input": 0x0343e730,
        "get_darkness_component": 0x01569fbc,
        "game_input_on_event": 0x02f97ef0,
        "app_on_input_event": 0x00c5d124,
        "input_manager_get_value": 0x064cdda0,
        "input_manager_singleton_ptr": 0x08b1e458,
        "camera_definition_singleton_ptr": 0x08b1e690,
        "movement_unlock_branch": 0x0343c470,
        "camera_should_move_store": 0x03323ff8,
        "core_input_mode_flag": 0x08b1e4c0,
    },
}

OUT_DIR = Path(__file__).resolve().parent.parent / "src" / "gen"
TOOL = "tools/gen_camera_addresses.py"


def render() -> dict[Path, str]:
    for store, vals in ADDRESSES.items():
        missing = [f for f, _, _ in FIELDS if f not in vals]
        if missing:
            raise SystemExit(f"{store} is missing: {', '.join(missing)}")

    struct_fields = "".join(f"    uint64_t {f};\n" for f, _, _ in FIELDS)
    macros = "".join(
        f"#define {m} (camera_addresses_get()->{f})\n" for f, m, _ in FIELDS
    )
    header = f'''/**
 * camera_addresses.h - camera/movement hook targets, chosen at runtime.
 *
 * GENERATED by {TOOL} -- do not edit by hand.
 *
 * One dylib serves both stores. The tables for every supported build are
 * compiled in and the right one is selected from the store detected at
 * runtime, the same way offset_table.c already picks its row.
 *
 * The macros are kept so callers read unchanged; each is now a lookup rather
 * than a constant, so none of them can be used where a compile-time constant
 * is required.
 *
 * A value of 0 means "not resolved for this build". Callers must treat 0 as
 * unavailable rather than as an address.
 */

#ifndef BG3SE_GEN_CAMERA_ADDRESSES_H
#define BG3SE_GEN_CAMERA_ADDRESSES_H

#include <stdint.h>

typedef struct {{
    const char *store;      /* "steam" or "gog", matched at runtime */
{struct_fields}}} CameraAddresses;

/* Never NULL: an unknown store yields an all-zero table, so every lookup
 * reports "not resolved" instead of returning a wild address. */
const CameraAddresses *camera_addresses_get(void);

{macros}
#endif /* BG3SE_GEN_CAMERA_ADDRESSES_H */
'''

    def table(store: str) -> str:
        vals = ADDRESSES[store]
        rows = "".join(
            f"    .{f:<32}= {vals[f]:#010x}ULL,   /* {d} */\n" if vals[f]
            else f"    .{f:<32}= 0ULL,          /* unresolved for this build */\n"
            for f, _, d in FIELDS
        )
        name = "kSteam" if store == "steam" else "kGog"
        return (f'static const CameraAddresses {name} = {{\n'
                f'    .store = "{store}",\n{rows}}};\n\n')

    source = f'''/**
 * camera_addresses.c - per-store camera/movement addresses.
 *
 * GENERATED by {TOOL} -- do not edit by hand.
 */

#include "camera_addresses.h"
#include "../core/version_detect.h"

#include <string.h>

''' + table("steam") + table("gog") + '''static const CameraAddresses kUnknown = { .store = "unknown" };

const CameraAddresses *camera_addresses_get(void) {
    /*
     * Cached after the first resolve. The store cannot change while the
     * process runs, and these are read from hot paths.
     */
    static const CameraAddresses *cached;
    if (cached) return cached;

    const char *store = version_detect_get_store();
    if (!store) return &kUnknown;          /* not detected yet; do not cache */

    if (strcmp(store, kGog.store) == 0)        cached = &kGog;
    else if (strcmp(store, kSteam.store) == 0) cached = &kSteam;
    else                                        return &kUnknown;

    return cached;
}
'''
    return {OUT_DIR / "camera_addresses.h": header,
            OUT_DIR / "camera_addresses.c": source}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="exit non-zero if the files are out of date")
    args = parser.parse_args(argv)

    stale = False
    for path, want in render().items():
        have = path.read_text() if path.exists() else None
        if have == want:
            continue
        stale = True
        if args.check:
            print(f"out of date: {path}", file=sys.stderr)
        else:
            path.write_text(want)
            print(f"wrote {path}")

    if args.check and stale:
        print(f"run: python3 {TOOL}", file=sys.stderr)
        return 1
    if args.check:
        print("camera_addresses is up to date")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
