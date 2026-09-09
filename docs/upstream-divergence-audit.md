# Osiris subsystem: divergence audit against Norbyte's BG3SE

**Baseline:** `/Users/jaredspigner/src/BG3SE/upstream/BG3Extender` (Norbyte's Windows
BG3 Script Extender). Every published mod is written and tested against that
implementation, so it — not this port's history — defines correct behaviour.
Where the port answers differently, the mod is the thing that breaks.

**Scope of this pass:** the whole Osiris surface — `Osi.*` dispatch, database
accessors, argument marshalling, `Ext.Osiris.*`, event dispatch, and the
story-lifecycle/caching machinery around them. Four upstream files were read end
to end (`Lua/Osiris/ValueHelpers.inl`, `Function.inl`, `FunctionProxy.inl`,
`CallbackManager.inl`, plus `Lua/Server/LuaServer.cpp` and
`Lua/Osiris/LuaNameResolver.inl`).

**Status legend:** ✅ fixed in this build · ⏳ deferred (Tier 2, listed at the
bottom with why) · 🍎 deliberate macOS-only deviation · 🍏 port-only extension
upstream does not have.

---

## 1. `Osi.<name>` resolution and dispatch

| # | Upstream | Port (before) | Mod impact | Fix | Status |
|---|----------|---------------|------------|-----|--------|
| 1 | `LuaNameResolver.inl:9-37` — an unknown symbol resolves to **nil** | `osi_index_handler` returned (and cached) a call closure for *any* name | `if Osi.Foo then` was true for every name, so mods took the wrong branch and only found out when the call raised | Unknown names return nil once the engine cache is populated and the story index is walked; before that a lazy closure is returned but **not cached** | ✅ `main.c:3838-3900` |
| 2 | `LuaNameResolver.inl:18-22` — wrong-case symbols resolve via a lowercase legacy index with a `COMPATIBILITY WARNING` | No case-insensitive path; wrong case silently produced a broken closure | A mod that writes `Osi.charactergetowner` got no diagnostic at all | Case-folded index over the name registry + linear fold over the engine cache; identical warning text, and the closure is bound to the canonical name | ✅ `main.c:3864`, `osiris_functions.c` (`osi_db_lookup_name_ci`, `osi_func_lookup_name_ci`) |
| 3 | `FunctionProxy.inl:28-39` — overload chosen by input count; no match raises | Name-only lookup, then pad/clamp arguments | `Osi.MakePlayer(guid)` silently ran the 3-arg overload as `(guid, "", 0)` — companions never became players | Overload chain + selection by input count; **over**-supplying raises upstream's message verbatim | ✅ (earlier this session) `main.c:3454` |
| 3b | Upstream's name cache holds every *declared* arity (it is built from the story's function table) | Ours is built by enumerating engine ids, so it can miss a declared arity | Refusing on incomplete knowledge broke calls that work on Windows: the shipped story itself calls `MakePlayer((CHARACTER)X, _Player)` (GustavDev `GLO_Origin_DarkUrge.txt:95`) against a signature we read as 3 inputs, and we raised `No function named 'MakePlayer' exists that can be called with 2 parameters` | **Under**-supplied calls pad the missing inputs with a typed zero/empty and log a WARN naming the padding; only over-supply is refused | ✅ `main.c:3454-3480` — proper fix is to build the overload chain from the name-index walk |
| 4 | `ValueHelpers.inl:301-340` — `tv.TypeId` = the **declared** type, conversion switches on its **root** type | Compared the declared id against `OSI_TYPE_INTEGER` exactly | BG3's integer-rooted aliases (`CRITICALITYTYPE`, `DEATHTYPE`, `ARMOURSET`, …) fell into the string branch: Osiris got a `char*` where it wanted an int | `osi_resolve_root_type_strict()` decides the branch, declared id is preserved on the value | ✅ `main.c:3567-3610` |
| 5 | `ValueHelpers.inl:260-295` — a wrong Lua type is an **error** (`Number expected for argument %d, got %s`, `String expected…`) | Silently coerced: non-numbers became `0`, non-strings became `""` | A `nil` typo reached Osiris as `""`/`0` and the call "succeeded" doing nothing | Same two error strings, same rules (numbers only for INTEGER/INTEGER64/REAL, strings only for STRING/GUIDSTRING) | ✅ `main.c:3583/3595/3605` |
| 6 | `ValueHelpers.inl:340` — unresolvable type raises `Unhandled Osi argument type %d` | Fell through to the string branch | Fatal engine assert when the root was not a string type | Raises with upstream's text | ✅ `main.c:3573` |
| 7 | `Function.inl:386` — a query with **no** OUT params returns a **boolean** | Returned integer `0`/`1` | `if Osi.Q(x) then` is true for `0` — every such test inverted | `lua_pushboolean` | ✅ `main.c:3736` |
| 8 | `Function.inl:407-430` — a failed query returns **one nil per OUT param** | Returned a single nil | `local a, b = Osi.Q(x)` left `b` bound to whatever was next on the stack | Pushes `numOut` nils | ✅ `main.c:3728` |
| 9 | `LuaPush.h:47-53` — `push(char const*)` pushes **nil** for `NULL` | Pushed `""` | `if Osi.GetOwner(c) then` was true for "no owner" (`""` is truthy) | NULL → nil; unreadable pointer → nil + one warning | ✅ `main.c:3245-3262` |
| 10 | Upstream has no string-length cap | 512-byte (`osi_value_to_lua`) and 256-byte (`osi_push_typed_value`) truncation | Long values (paths, dialogue keys) silently truncated | Both raised to 4096 | ✅ `main.c:3253`, `main.c:1553` |
| 11 | Column values are read by their **root** type | `osi_push_typed_value` switched on the declared id | Integer-rooted alias columns were read as string handles → `""` for every row | Root-type switch, same as `osi_value_to_lua` | ✅ `main.c:1534` |
| 12 | Upstream has exactly one implementation per symbol: the engine's | Port bound hand-rolled C functions for `GetDistanceTo`, `SpeakerGetDialog`, `DialogGetNumberOfInvolvedPlayers`, `DialogRequestStop`, `QRY_StartDialog_Fixed`, plus a `GetHostCharacter` **global** | Each answered plausibly without asking Osiris: distance `0.0` ("on top of each other"), dialog getters that ignored their arguments, a dialog starter that started nothing, and a host heuristic ("first GUID not starting with `S_Player_`") that shadowed the real query | All six unbound; they route through the generic dispatcher, which reads the game's parameter defs | ✅ `main.c:3973`, `main.c:4015` |
| 13 | `ValueHelpers.inl` error index is the **Lua stack index** (self at 1) | — | — | Port reports the **argument position** (1-based) instead; more accurate for a mod author, and no mod parses these | 🍎 documented deviation |
| 14 | Story-insert type check message | `Number expected for argument %d **of '%s'**, got %s` | — | Kept: names the symbol, which upstream's does not | 🍎 `main.c:2752` |

## 2. Database accessors (`Osi.DB_*`)

| # | Upstream | Port (before) | Mod impact | Fix | Status |
|---|----------|---------------|------------|-----|--------|
| 15 | `FunctionProxy.inl:57-73` — `:Get(...)` with an arity no overload has raises `No database named '%s(%d)' exists` | Fell back to the lowest-arity overload, or returned `{}` | A typo or wrong arity looked like "a database with no rows"; a wrong-arity Get silently read a different database | Exact-arity lookup, then the arity-0 (unreadable-signature) entry, then raise — but only once the story is walked, so early calls still return `{}` | ✅ `main.c:2996-3011` |
| 16 | `FunctionProxy.inl:68-70` — a non-database symbol raises `'%s(%d)' is not a database` | Warned and returned `{}` | Same silent-empty confusion | Raises with upstream's text | ✅ `main.c:3011` |
| 17 | `Function.inl:318-347` — `:Delete(...)` treats `nil` as a **wildcard** and deletes **every** matching row (`CReteNode::DeleteTuple`) | Rejected `nil` outright, and erased only the first match | `Osi.DB_X:Delete(guid, nil)` errored; a multi-row delete left every row but the first | Wildcards accepted (the matcher already supported them) and the erase loop runs until no row matches | ✅ `main.c:2287-2370` |
| 18 | `FunctionProxy.inl:76-92` — `:Delete` on an unknown name raises | Warned and returned | Silent no-op | Raises once the story is walked | ✅ `main.c:2267` |
| 19 | `FunctionProxy.inl:5-25` — the proxy also exposes `:Exists(arity)`, `:Type(arity)`, `.Arities`, `.InputArities`, `__tostring`, and raises `Not a valid OsiFunction method or property: %s` for anything else | Port exposes `:Get`, `:Delete` and insert only | A mod probing `Osi.DB_X:Exists(2)` gets `attempt to call a nil value` | — | ⏳ Tier 2 |

## 3. `Ext.Osiris` and event dispatch

| # | Upstream | Port (before) | Mod impact | Fix | Status |
|---|----------|---------------|------------|-----|--------|
| 20 | `LuaServer.cpp:82-89` — `RegisterListener` returns a subscription id; `UnregisterListener(id)` returns bool | No `UnregisterListener` at all | A mod that unsubscribes on teardown hit a nil call; listeners leaked across reloads | Added, with tombstones (`callback_ref = LUA_NOREF`) and `false` for a dead/unknown id | ✅ `lua_osiris.c:111` |
| 21 | `LuaServer.cpp:66-74` — timing must be one of four values, else `Hook type must be 'before', 'beforeDelete', 'after' or 'afterDelete'` | Any string accepted | A typo registered a listener that could never fire, silently | Same validation, same message | ✅ `lua_osiris.c:80` |
| 22 | `CallbackManager.inl:48-60` — no cap on subscriptions | Fixed array of 512; the 513th was dropped | Large profiles silently lost listeners | Growable table (doubling `realloc`), 512 is now just the first allocation | ✅ `lua_osiris.c:44-70` |
| 23 | `CallbackManager.inl:177/249` — handler errors go through `CallWithTraceback` and are logged at **error** level | `lua_pcall` with no message handler, logged at INFO | Errors had no traceback and were buried in a DEBUG-volume log | Traceback message handler + `LOG_OSIRIS_ERROR("Osiris event handler failed: …")` on both dispatch paths | ✅ `lua_osiris.c:271`, `main.c:5529` |
| 24 | Handlers may register/unregister during dispatch | The dispatch loop held a pointer into the listener array across the callback | Registering a listener from inside a handler could reallocate the array under the loop | Callback ref and arity are copied before the call | ✅ `main.c:5492` |
| 25 | `CallbackManager.inl:276-340` — subscriptions are bound to **RETE nodes** at StoryLoaded, so `DB_*`, `PROC_*` and `QRY_*` names fire; a name with no story symbol logs `Couldn't register Osiris subscriber for X/N: Symbol not found in story.` | Only engine events dispatch; no node VMT hooks, and no diagnostic for a name that will never fire | **A mod listening on a database insert/delete or a PROC/QRY never runs.** This is the largest remaining gap | — | ⏳ Tier 2 (highest priority) |
| 26 | `Ext.Osiris` is exactly `RegisterListener` + `UnregisterListener` (`LuaServer.cpp:91-101`) | Port adds `NewCall`, `NewQuery`, `NewEvent`, `RaiseEvent`, `GetCustomFunctions` | None (nothing upstream calls them) — but they were documented as if they were parity APIs | Documented as port-only in `api-reference.md` and the IDE helper stubs | 🍏 |
| 27 | `Ext.Osiris` is registered **only** in the server VM; the client VM has no `Osi` table | Registered in every VM; the context check only logs | A mod branching on `Osi ~= nil` treats the client as a server | — | ⏳ Tier 2 |

## 4. Story lifecycle and caching

| # | Upstream | Port (before) | Mod impact | Fix | Status |
|---|----------|---------------|------------|-----|--------|
| 27b | Upstream's name cache is empty between story teardown and StoryLoaded, so `Osi.<name>` is `nil` and a call in that window is an ordinary Lua error | Dispatched the **previous story's** cached `OsiFunctionId` into a half-built Osiris | **Hard crash.** A mod timer firing `Osi.IsDead` 0.4 s into "load another save" threw out of `COsiArgumentDesc::GetDataSrcPtr` → `std::terminate` → SIGABRT on the ServerWorker thread (`Baldur's Gate 3-2026-09-07-014637.ips`, log 01:46:30.166) | `osi_story_rebuilding()` refuses dispatch between UnloadSession/LoadSession/BuildStory/ReloadStory and the Sync walk, raising `Osiris is not available while the story is loading`. Measured over a normal load: zero legitimate Osi calls happen in that window | ✅ `main.c:2097-2124` |
| 28 | The name cache is rebuilt from scratch at every StoryLoaded | Cache refresh had a **lifetime** budget of 12 attempts | After a long first session, every later session's lookup miss was permanent | `osi_func_refresh_reset()` on story teardown, next to `osi_db_invalidate` | ✅ `osiris_functions.c`, `main.c:5735` |
| 29 | `GenerateOsiHelpers()` writes one global per symbol at story load | Lazy `_G.__index` resolver | None — the lazy form also covers symbols registered after bootstrap | — | 🍎 deliberate (better) |
| 30 | Upstream hooks Osiris `Merge` to invalidate caches on savegame merge | Invalidation is driven by game-state transitions only | A mid-session story merge could leave stale defs | — | ⏳ Tier 2 |
| 31 | Upstream wraps `Call`, `Query`, `Error` and `Assert` | Port wraps `Call`/`Query` dispatch only | Osiris-side errors and asserts are not surfaced to Lua | — | ⏳ Tier 2 |
| 32 | `RestrictOsiris` blocks `Osi.*` outside legal contexts | No restriction | A mod calling Osiris from the wrong context gets undefined behaviour instead of a clear error | — | ⏳ Tier 2 |
| 33 | User queries are dispatched through their node | Port raises "not supported yet" for user queries with no dispatch id | A mod calling a story `QRY_*` gets an error | — | ⏳ Tier 2 |

---

## Deferred (Tier 2), in the order they should be taken

1. **#25 — RETE node listener hooks.** Subscribing to `DB_*`/`PROC_*`/`QRY_*`
   requires hooking the node VMTs at story load the way `CallbackManager.inl`
   does. Until then those subscriptions register and never fire. Everything
   else on this list is smaller.

   Groundwork done (2026-09-07), see `ghidra/offsets/OSIRIS_RETE_VMT.md`:
   - Databases are `CReteFact` nodes, procs are `CReteEvent` nodes; both inherit
     the relevant virtuals from `CReteStartNode`, so two vtables cover both.
   - The entry points are **`CReteStartNode::Add` (vptr+0x58)** and **`::Del`
     (vptr+0x60)** — *not* upstream's slot numbers. The port had copied
     Norbyte's Windows `InsertTuple` slot (+0x50), which on this build is
     `Adaptor()`; hooking it would have replaced the wrong virtual silently.
   - The vtables are static in `libOsiris`, so they can be patched once and
     survive story reloads (unlike the per-story nodes themselves).
   - Still needed before hooking: the `COsiVarIDValuePairList` tuple layout
     (upstream marshals a `TuplePtrLL`, which is a different type here), and
     `mprotect` on the vtable page.
   - Also corrected in passing: `osi_node_is_data_node` was calling
     `pDBase()` through a `bool` cast, reading only the low byte of a returned
     pointer — a `CReteDBase*` ending in `0x00` would have sent a database down
     the proc path.
2. **#33 / #31** — user-query dispatch and the `Error`/`Assert` wrappers; both
   need the same node plumbing as #25.
3. **#27** — hide `Osi`/`Ext.Osiris` in the client VM (needs the dual-VM split
   verified first; historically this port ran one VM for both halves).
4. **#19** — the remaining `OsiFunctionName` methods (`Exists`, `Type`,
   `Arities`, `InputArities`, `__tostring`, and the "not a valid method" error).
5. **#30** — invalidate on the Osiris `Merge` hook rather than only on state
   transitions.
6. **#32** — `RestrictOsiris` context gating.

## What to verify in-game after the next relaunch

- `!osi_info MakePlayer` lists three overloads; the dispatch line reads
  `overloads=3, exact=1`.
- `Osi.NoSuchFunction` is `nil` (and `Osi.DB_NoSuchDatabase` is `nil`).
- `Osi.DB_Players:Get(guid)` still returns rows; `Osi.DB_Players:Get(1, 2, 3)`
  raises `No database named 'DB_Players(3)' exists`.
- `Osi.IsInCombat(char)` still returns an integer (it has an OUT param), while a
  genuinely 0-OUT query returns `true`/`false`.
- `Osi.CharacterGetOwner(<ownerless char>)` returns `nil`, not `""`.
- `Ext.Osiris.RegisterListener(...)` returns an id and
  `Ext.Osiris.UnregisterListener(id)` returns `true`, then `false` on a repeat.
- `Osi.MakePlayer(nil)` raises `String expected for argument 1, got nil` rather
  than doing nothing.
- **Load a second save from inside a running session** — the crash from
  2026-09-07 01:46. Expect no abort, and (if a mod timer fires mid-load) a
  `Osiris is not available while the story is loading` line in the log instead.
