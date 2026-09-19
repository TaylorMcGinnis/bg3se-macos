# Immersive camera on GOG: what it took

Outcome record for branch `gog-camera-port`. The plan
(`2026-09-18-001`) covered the addresses. This covers everything the plan did
not predict, because that was most of the work.

## Result

Camera and W/A/S/D movement both work on GOG 4.1.1.7398727. The user path is:
install the extender, add the `.pak` with a mod manager, press Play. No launch
flags, no keybind editing.

## The addresses were the easy part

All 12 constants from the plan resolved and every one proved correct in the
running game. The binary is not stripped, so symbol lookup did the work and
the prologue checks confirmed it. Nothing in that phase needed a second
attempt.

Two more turned up later, neither predicted:

| Constant | Why it was missed |
|---|---|
| `client_level_mgr_ptr` | The camera reads the *client* level's AiGrid. `level_get_current()` walks `esv::LevelManager`, which only mirrors it in single-player. |
| `EoCGlobalSwitches+0xe98` | Not an address to resolve. A setting whose meaning had to be worked out. |

## What actually blocked movement

Three things, each of which looked like the others.

**1. The task selector.** `CanExecute` loads `EoCGlobalSwitches` and reads one
dword. When it is zero it branches *past* the `GetRotatedInput` call. So the
mod's `CanExecute` patch could never produce movement on its own: the call was
skipped, not rejected. Read live on a stock install: zero.

This is configuration, not a build difference. It explains "works on Steam"
with no GOG/Steam divergence at all.

**2. Character movement ships unbound.** From the game's own Mac defaults in
`GamePlatform.pak`:

    "CameraForward":        [ "key:w", "key:up" ],
    "CharacterMoveForward": [],

The actions exist. They are unbound, and the Options screen has no entry for
them, so a player cannot bind them by hand. The extender now derives them from
the camera bindings, which is what Ch4nKyy's BG3WASD does on Windows.

**3. Mod discovery.** The mod shipped with no `meta.lsx` and no UUID, so it
could not appear in a load order at all.

## Wrong turns worth recording

**Keybind conflict.** I believed the camera holding W/A/S/D blocked the
character bindings, and spent time clearing them with `INVALID:unknown`.
Testing with both bound proved it irrelevant. BG3WASD's own comment said so and
I read past it.

**Mouse look.** I predicted `IsInSelectorMode` was the gate and that Caps Lock
mouse-look would clear it. Measured: the probe returned false 1567 times while
the task still never ran. Wrong twice, corrected both times by measurement
rather than argument.

The useful instrument was a call counter on `GetRotatedInput`. `CanExecute`
calls that function itself, so a count of zero proved `CanExecute` was bailing
out before reaching it, which narrowed the search to a handful of instructions.

## Defects introduced, and found

- **SIGSEGV in `storage_container_for_world`** (`ccc38f5`). I copied the
  upstream fork's raw dereference of a caller-supplied world pointer and lost
  this tree's `safe_memory_read_pointer` convention. Timing-dependent, so it
  passed several sessions before crashing during a save load.
- **The same pattern in `component_lookup_by_index_in_world`** (`f7c43cd`).
  Pre-existing, latent while nothing passed a world, newly reachable because
  the camera passes one from three call sites.
- **Buffer sizing and empty bindings** (`0839b67`). Found rereading my own code.
- **Gate on an unpacked folder** (`d47b48b`). Broke the moment the mod became a
  `.pak`, and was masked because an earlier run had already fixed the file.

## Silent failures a user will hit

Every one of these presents as "the mod does nothing":

| Failure | Symptom |
|---|---|
| `meta.lsx` with `UUID type="guid"` | BG3 drops the mod from the load order on exit. No error. |
| Launching from Galaxy's Play button | No `DYLD_INSERT_LIBRARIES`, no mods, no log file at all. |
| Input Monitoring not granted | Modifier keys work, ordinary keys do not. |
| Camera keys unbound by the player | Would have cleared character movement, with no way back. Guarded now. |

The `meta.lsx` one is the worst, because a mod manager accepts the file that
BG3 rejects. `scripts/build-pak.sh` in the mod fork now checks it at build
time.

## Still unverified

- What Larian call the `+0xe98` switch, and what else reads it. `config.lsf`
  has not been written since we started setting it, which suggests it is
  memory-only, but that is not proof.
- Whether a keybind edit in Options triggers `App::LoadInputScheme`. If it
  does, hooking it would make binding changes apply without a restart.
- Why movement needs one click after a save load before W/A/S/D responds.
- The Steam addresses for `CORE_INPUT_MODE_FLAG` and
  `INPUT_IS_IN_SELECTOR_MODE`. Both are 0 on Steam, which disables the
  diagnostics rather than guessing.
