# API Reference

Complete documentation for BG3SE-macOS Lua APIs.

## Status Legend

| Symbol | Meaning |
|--------|---------|
| ✅ | Fully working |
| ⚠️ | Framework exists, partial implementation |
| ⏳ | Stub only |

## Context Annotations

APIs are marked with their context availability:

| Symbol | Meaning |
|--------|---------|
| **B** | Both - Works in server and client contexts |
| **S** | Server-only - Requires server context (BootstrapServer.lua) |
| **C** | Client-only - Requires client context (BootstrapClient.lua) |

**Note:** Most BG3SE-macOS APIs work in both contexts. Server-only restrictions apply to Osiris functions and stat modification. Client-only restrictions apply to input handling.

---

## Userdata Lifetime Scoping

BG3SE-macOS implements lifetime scoping for userdata objects (Entities, Components, StatsObjects). This prevents accessing stale objects that may have been destroyed or modified by the game engine.

### How It Works

Each console command or Lua callback runs in its own **lifetime scope**. Objects created within a scope are valid only for that scope's duration. When the scope ends, all objects created in it become **expired**.

```lua
-- Scope 1: First console command
local entity = Ext.Entity.Get(GetHostCharacter())
stored = entity  -- Store for later (BAD PRACTICE)

-- Scope 2: Second console command
_P(stored)              -- Shows: Entity(0x...) [EXPIRED]
stored.Health.Hp        -- ERROR: "Lifetime of Entity has expired"

-- Correct usage: Re-fetch in each scope
local entity = Ext.Entity.Get(GetHostCharacter())
_P(entity.Health.Hp)    -- Works!
```

### Affected Types

| Type | Description |
|------|-------------|
| Entity | From `Ext.Entity.Get()` |
| Component | From `entity.Health`, `entity:GetComponent()`, etc. |
| ArrayProxy | From `entity.Tag.Tags`, `entity.SpellBook.Spells`, etc. |
| StatsObject | From `Ext.Stats.Get()` |

### Detecting Expired Objects

- `tostring(obj)` shows `[EXPIRED]` suffix for expired objects
- Property access on expired objects throws a clear error message
- The error message instructs you to re-fetch the object

### Best Practices

1. **Don't store userdata** in global tables for later use
2. **Re-fetch objects** at the start of each callback or command
3. **Complete work within one scope** when possible

---

## Ext Namespace

### Core Functions

| API | Status | Ctx | Description |
|-----|--------|-----|-------------|
| `Ext.Print(...)` | ✅ | B | Print to BG3SE log |
| `Ext.GetVersion()` | ✅ | B | Returns version string |
| `Ext.IsClient()` | ✅ | B | Returns true |
| `Ext.IsServer()` | ✅ | B | Returns false |
| `Ext.Require(path)` | ✅ | B | Load Lua module relative to mod |

### Ext.IO

| API | Status | Ctx | Description |
|-----|--------|-----|-------------|
| `Ext.IO.LoadFile(path)` | ✅ | B | Read file contents |
| `Ext.IO.SaveFile(path, content)` | ✅ | B | Write file contents |

### Ext.Json

| API | Status | Ctx | Description |
|-----|--------|-----|-------------|
| `Ext.Json.Parse(json)` | ✅ | B | Parse JSON to Lua table |
| `Ext.Json.Stringify(table)` | ✅ | B | Convert Lua table to JSON |

---

## Ext.Entity

Entity Component System access for querying game objects.

| API | Status | Ctx | Description |
|-----|--------|-----|-------------|
| `Ext.Entity.Get(guid)` | ✅ | B | Look up entity by GUID string |
| `Ext.Entity.IsReady()` | ✅ | B | Check if entity system ready |
| `entity.Transform` | ✅ | B | Get transform component (Position, Rotation, Scale) |
| `entity:GetComponent(name)` | ✅ | B | Get component by name (short or full) |
| `entity:IsAlive()` | ✅ | B | Check if entity is valid |
| `entity:GetHandle()` | ✅ | B | Get raw EntityHandle value |

### Component Property Access

Components with property layouts return proxy userdata that supports direct property access and iteration with `pairs()`.

#### Health Component

| Property | Type | Description |
|----------|------|-------------|
| `entity.Health.Hp` | int32 | Current HP |
| `entity.Health.MaxHp` | int32 | Maximum HP |
| `entity.Health.TemporaryHp` | int32 | Temporary HP |
| `entity.Health.MaxTemporaryHp` | int32 | Max temporary HP |
| `entity.Health.IsInvulnerable` | bool | Invulnerability flag |

#### BaseHp Component

| Property | Type | Description |
|----------|------|-------------|
| `entity.BaseHp.Vitality` | int32 | Base vitality value |
| `entity.BaseHp.VitalityBoost` | int32 | Vitality boost modifier |

#### Stats Component

| Property | Type | Description |
|----------|------|-------------|
| `entity.Stats.InitiativeBonus` | int32 | Initiative modifier |
| `entity.Stats.Abilities` | int32[7] | Array: STR, DEX, CON, INT, WIS, CHA, unused |
| `entity.Stats.AbilityModifiers` | int32[7] | Computed ability modifiers |
| `entity.Stats.Skills` | int32[18] | All skill values |
| `entity.Stats.ProficiencyBonus` | int32 | Proficiency bonus |
| `entity.Stats.SpellCastingAbility` | uint8 | Primary casting ability |

#### Level Component

| Property | Type | Description |
|----------|------|-------------|
| `entity.Level.LevelHandle` | EntityHandle | Handle to level entity (hex string) |
| `entity.Level.LevelName` | FixedString | Level name index (raw uint32) |

#### Data Component

| Property | Type | Description |
|----------|------|-------------|
| `entity.Data.Weight` | int32 | Entity weight (in game units) |
| `entity.Data.StatsId` | FixedString | Stats ID index (raw uint32) |
| `entity.Data.StepsType` | uint32 | Steps type value |

#### Transform Component

| Property | Type | Description |
|----------|------|-------------|
| `entity.Transform.Position` | table | {x, y, z} world position |
| `entity.Transform.Rotation` | table | {x, y, z, w} quaternion rotation |
| `entity.Transform.Scale` | table | {x, y, z} scale factors |

**Example:**
```lua
local entity = Ext.Entity.Get(GetHostCharacter())
if entity then
    -- Health
    _P("HP: " .. entity.Health.Hp .. "/" .. entity.Health.MaxHp)

    -- Stats
    _P("Proficiency: " .. entity.Stats.ProficiencyBonus)

    -- Level
    _P("Level Handle: " .. entity.Level.LevelHandle)

    -- Data
    _P("Weight: " .. entity.Data.Weight)

    -- Iterate all properties on a component
    for k, v in pairs(entity.Stats) do
        _P(k .. " = " .. tostring(v))
    end
end
```

### Array Components (v0.35.0+)

Components with `Array<T>` fields expose **ArrayProxy** userdata that supports standard Lua iteration.

#### Supported Arrays

| Component | Property | Element Type | Fields |
|-----------|----------|--------------|--------|
| `Tag` | `Tags` | GUID | String (UUID format) |
| `Classes` | `Classes` | ClassInfo | `ClassUUID`, `SubClassUUID`, `Level` |
| `SpellBook` | `Spells` | SpellData | `SpellId`, `__ptr`, `__index`, `__size` |
| `SpellContainer` | `Spells` | SpellMeta | `__ptr`, `__index`, `__size` |
| `PassiveContainer` | `Passives` | EntityHandle | String (hex format) |
| `BoostsContainer` | `Boosts` | BoostEntry | `Type`, `BoostCount`, `__ptr` |

#### ArrayProxy Metamethods

| Operation | Example | Description |
|-----------|---------|-------------|
| Length | `#array` | Returns element count |
| Index | `array[1]` | Access element (1-based) |
| Iteration | `ipairs(array)` | Standard Lua iteration |
| Pairs | `pairs(array)` | Key-value iteration |
| ToString | `tostring(array)` | Debug: `Array[22](0x12345678)` |

#### Examples

```lua
local entity = Ext.Entity.Get(GetHostCharacter())

-- Iterate all tags (GUIDs)
for i, tag in ipairs(entity.Tag.Tags) do
    _P("Tag " .. i .. ": " .. tag)
end

-- Access class information
for i, class in ipairs(entity.Classes.Classes) do
    _P("Class: " .. class.ClassUUID)
    _P("  Subclass: " .. class.SubClassUUID)
    _P("  Level: " .. class.Level)
end

-- Check boost types
for i, boost in ipairs(entity.BoostsContainer.Boosts) do
    _P("Boost Type " .. boost.Type .. " has " .. boost.BoostCount .. " entries")
end

-- Get passive entity handles
for i, handle in ipairs(entity.PassiveContainer.Passives) do
    _P("Passive handle: " .. handle)
end

-- Iterate spells with metadata
local spells = entity.SpellBook.Spells
_P("Total spells: " .. #spells)
for i, spell in ipairs(spells) do
    _P("Spell " .. i .. ": ID=" .. tostring(spell.SpellId))
end
```

### Debug/Discovery Functions

| API | Status | Ctx | Description |
|-----|--------|-----|-------------|
| `Ext.Entity.DumpComponentRegistry()` | ✅ | B | Dump all registered components |
| `Ext.Entity.DumpStorage(handle)` | ✅ | B | Test TryGet and dump EntityStorageData |
| `Ext.Entity.DiscoverTypeIds()` | ✅ | B | Discover indices from TypeId globals |
| `Ext.Entity.DumpTypeIds()` | ✅ | B | Dump all known TypeId addresses |
| `Ext.Entity.RegisterComponent(name, idx, size)` | ✅ | B | Register discovered component |
| `Ext.Entity.LookupComponent(name)` | ✅ | B | Look up component info by name |

---

## Ext.Stats

Access to the RPGStats system (weapons, armor, spells, etc.).

| API | Status | Ctx | Description |
|-----|--------|-----|-------------|
| `Ext.Stats.Get(name)` | ✅ | B | Get StatsObject by name |
| `Ext.Stats.GetAll(type?)` | ✅ | B | Get all stat names, optionally by type |
| `Ext.Stats.Create(name, type, template?)` | ✅ | S | Create new stat object at runtime |
| `Ext.Stats.Sync(name)` | ⚠️ | S | Mark stat as synced (prototype manager sync pending) |
| `Ext.Stats.IsReady()` | ✅ | B | Check if stats system ready |
| `Ext.Stats.DumpTypes()` | ✅ | B | Print all stat types to log |

### Ext.Stats.Create

Creates a new stat object at runtime. The stat is stored in a shadow registry and accessible via `Ext.Stats.Get()`.

**Parameters:**
- `name` (string) - Unique name for the new stat
- `type` (string) - Stat type: "Weapon", "Armor", "SpellData", "StatusData", "PassiveData", etc.
- `template` (string, optional) - Existing stat to copy properties from

**Returns:** StatsObject or nil (if name exists, type invalid, or system not ready)

**Example:**
```lua
-- Create a new weapon from scratch
local sword = Ext.Stats.Create("MyMod_CustomSword", "Weapon")
sword.Damage = "2d8"
sword.DamageType = "Slashing"

-- Create based on existing stat
local betterSword = Ext.Stats.Create("MyMod_BetterSword", "Weapon", "WPN_Longsword")
betterSword.Damage = "3d8"  -- Override template value

-- Call Sync when done modifying
Ext.Stats.Sync("MyMod_CustomSword")
```

**Note:** Created stats exist in memory and support property read/write. Prototype manager sync (for game to use stats in spawning/casting) is pending implementation.

### StatsObject Properties

| Property | Status | Ctx | Description |
|----------|--------|-----|-------------|
| `stat.Name` | ✅ | B | Read-only stat name |
| `stat.Type` | ✅ | B | Read-only stat type |
| `stat.Level` | ✅ | B | Read-only stat level |
| `stat.Using` | ✅ | B | Read-only parent stat |
| `stat:GetProperty(name)` | ✅ | B | Get property value |
| `stat:SetProperty(name, value)` | ✅ | S | Set property value |
| `stat:Dump()` | ✅ | B | Print stat contents to log |

**Example:**
```lua
local sword = Ext.Stats.Get("WPN_Longsword")
Ext.Print(sword.Damage)  -- "1d8"
sword.Damage = "2d6"     -- Modify at runtime
```

---

## Ext.Enums

Type-safe enum and bitfield access matching Windows BG3SE behavior.

### Enum Access

```lua
-- Access enum values via Ext.Enums.<EnumName>.<Label>
local dt = Ext.Enums.DamageType.Fire
_P(dt.Label)      -- "Fire"
_P(dt.Value)      -- 7
_P(dt.EnumName)   -- "DamageType"

-- Flexible comparison
dt == "Fire"      -- true (label match)
dt == 7           -- true (value match)
dt == Ext.Enums.DamageType.Fire  -- true
```

### Bitfield Access

```lua
-- Access bitfield values
local bf = Ext.Enums.AttributeFlags.Backstab
_P(bf.__Labels)   -- {"Backstab"}
_P(bf.__Value)    -- 65536

-- Query individual flags
bf.Backstab       -- true
bf.Torch          -- false

-- Bitwise operations
local combined = bf | Ext.Enums.AttributeFlags.Torch
_P(#combined)     -- 2 (popcount)
_P(tostring(combined))  -- "Backstab, Torch"
```

### Available Enums

| Enum | Type | Description |
|------|------|-------------|
| `DamageType` | Enum | None, Slashing, Piercing, Bludgeoning, Acid, Thunder, Necrotic, Fire, Lightning, Cold, Psychic, Poison, Radiant, Force |
| `AbilityId` | Enum | None, Strength, Dexterity, Constitution, Intelligence, Wisdom, Charisma |
| `SkillId` | Enum | Deception, Intimidation, Performance, Persuasion, Acrobatics, SleightOfHand, Stealth, Arcana, History, Investigation, Nature, Religion, Athletics, AnimalHandling, Insight, Medicine, Perception, Survival |
| `StatusType` | Enum | DYING, HEAL, KNOCKED_DOWN, BOOST, FEAR, INVISIBLE, INCAPACITATED, POLYMORPHED, DOWNED, etc. |
| `SurfaceType` | Enum | None, Water, WaterElectrified, WaterFrozen, Blood, Poison, Oil, Lava, Fire, Acid, etc. |
| `SpellSchoolId` | Enum | None, Abjuration, Conjuration, Divination, Enchantment, Evocation, Illusion, Necromancy, Transmutation |
| `WeaponType` | Enum | None, Sword, Club, Axe, Staff, Bow, Crossbow, Spear, Knife, Wand, Arrow, Rifle |
| `ArmorType` | Enum | None, Cloth, Padded, Leather, StuddedLeather, Hide, ChainShirt, ScaleMail, BreastPlate, HalfPlate, RingMail, ChainMail, Splint, Plate |
| `ItemSlot` | Enum | Helmet, Breast, Cloak, MeleeMainHand, MeleeOffHand, RangedMainHand, RangedOffHand, Ring, Boots, Gloves, Amulet, etc. |
| `ItemDataRarity` | Enum | Common, Unique, Uncommon, Rare, Epic, Legendary, Divine |
| `SpellType` | Enum | None, Zone, MultiStrike, Projectile, ProjectileStrike, Rush, Shout, Storm, Target, Teleportation, Wall, Throw |
| `AttributeFlags` | Bitfield | SlippingImmunity, Torch, Arrow, Unbreakable, Grounded, Floating, ThrownImmunity, InvisibilityImmunity, Backstab, BackstabImmunity, etc. |
| `WeaponFlags` | Bitfield | Light, Ammunition, Finesse, Heavy, Loading, Range, Reach, Thrown, Twohanded, Versatile, Melee, Magical, etc. |
| `DamageFlags` | Bitfield | Hit, Dodge, Miss, Critical, Backstab, Invisible, Magical, Invulnerable, SavingThrow, KillingBlow, etc. |

---

## Ext.Types

Full reflection API for type introspection across all registered types (userdata, components, enums).

### Functions

| Function | Ctx | Description |
|----------|-----|-------------|
| `GetAllTypes()` | B | Returns array of all ~2050 registered type names |
| `GetTypeInfo(name)` | B | Returns rich metadata table for a type |
| `GetObjectType(obj)` | B | Returns type name string for a userdata object |
| `TypeOf(obj)` | B | Returns full type info table for an object |
| `IsA(obj, typeName)` | B | Returns true if object is or inherits from type |
| `Validate(obj)` | B | Returns true if userdata is still valid |
| `GetComponentLayout(name)` | B | Returns property layout for a component |
| `GetAllLayouts()` | B | Returns array of component names with layouts |
| `GenerateIdeHelpers(filename?)` | B | Generate LuaLS type annotations for VS Code |

### GetAllTypes()

```lua
-- Returns all registered types (userdata + 1999 components + 50+ enums)
local types = Ext.Types.GetAllTypes()
_P("Total types: " .. #types)  -- ~2050

-- Filter by namespace
for _, name in ipairs(types) do
    if name:match("^eoc::") then
        _P("EoC component: " .. name)
    end
end
```

### GetTypeInfo(name)

Returns rich metadata depending on type kind:

```lua
-- Component type info
local info = Ext.Types.GetTypeInfo("eoc::HealthComponent")
_P(info.Kind)       -- "Component"
_P(info.Size)       -- 24 (bytes)
_P(info.TypeIndex)  -- 575
_P(info.IsOneFrame) -- false
_P(info.IsProxy)    -- false
_P(info.Discovered) -- true

-- Enum type info
local info = Ext.Types.GetTypeInfo("DamageType")
_P(info.Kind)        -- "Enum"
_P(info.ValueCount)  -- 14
_P(info.Values.Fire) -- 7
for _, label in ipairs(info.Labels) do
    _P(label)
end

-- Bitfield type info
local info = Ext.Types.GetTypeInfo("AttributeFlags")
_P(info.Kind)         -- "Bitfield"
_P(info.AllowedFlags) -- Combined mask of all valid flags

-- Userdata type info
local info = Ext.Types.GetTypeInfo("bg3se.Entity")
_P(info.Kind)        -- "Userdata"
_P(info.HasMetatable)-- true
_P(info.MethodCount) -- Number of methods
```

### TypeOf(obj)

```lua
-- Get type info for an object directly
local entity = Ext.Entity.Get(GetHostCharacter())
local info = Ext.Types.TypeOf(entity)
_P(info.Name)  -- "bg3se.Entity"
_P(info.Kind)  -- "Userdata"
```

### IsA(obj, typeName)

```lua
local entity = Ext.Entity.Get(GetHostCharacter())

-- Direct type match
Ext.Types.IsA(entity, "bg3se.Entity")  -- true

-- Namespace match
Ext.Types.IsA(entity, "bg3se")  -- true

-- Substring match (inheritance pattern)
local stat = Ext.Stats.Get("WPN_Longsword")
Ext.Types.IsA(stat, "StatsObject")  -- true
```

### GetComponentLayout(name)

Returns property layout definition for components with known field offsets:

```lua
local layout = Ext.Types.GetComponentLayout("eoc::HealthComponent")
_P(layout.Name)          -- "eoc::HealthComponent"
_P(layout.ShortName)     -- "Health"
_P(layout.Size)          -- 40 (bytes)
_P(layout.PropertyCount) -- 5

-- Inspect properties
for _, prop in ipairs(layout.Properties) do
    _P(string.format("  %s: %s (offset 0x%x)", prop.Name, prop.Type, prop.Offset))
end
-- Output:
--   Hp: integer (offset 0x0)
--   MaxHp: integer (offset 0x4)
--   TemporaryHp: integer (offset 0x8)
--   MaxTemporaryHp: integer (offset 0xc)
--   IsInvulnerable: boolean (offset 0x20)
```

### GetAllLayouts()

```lua
-- Get all components with property layouts (~534)
local layouts = Ext.Types.GetAllLayouts()
_P("Components with layouts: " .. #layouts)

for i, name in ipairs(layouts) do
    if i <= 10 then _P(name) end
end
```

### GenerateIdeHelpers(filename?)

Generates LuaLS-compatible type annotations for VS Code IntelliSense:

```lua
-- Generate and save to file
Ext.Types.GenerateIdeHelpers("ExtIdeHelpers.lua")
-- Saved to: ~/Library/Application Support/BG3SE/ExtIdeHelpers.lua

-- Or get content as string without saving
local content = Ext.Types.GenerateIdeHelpers()
_P("Generated " .. #content .. " bytes")
```

**Output includes:**
- `---@class` definitions for all 1,999 components
- `---@field` annotations for 534 components with known layouts
- `---@alias` for all 14 enum types
- `Ext.*` namespace with function signatures
- `Osi.*` and `Mods` global stubs

**VS Code setup:**

1. Generate helpers: `!ide_helpers` in console
2. Copy `ExtIdeHelpers.lua` to your mod folder
3. Create `.luarc.json`:
```json
{
  "runtime.version": "Lua 5.4",
  "workspace.library": ["./ExtIdeHelpers.lua"],
  "diagnostics.globals": ["Ext", "Osi", "Mods"]
}
```

---

## Ext.Events

Event subscription system for game lifecycle events.

> **⚠️ API Note:** The API uses **per-event objects** with colon syntax:
> ```lua
> Ext.Events.LevelUp:Subscribe(fn)     -- ✅ Correct
> Ext.Events.Subscribe("LevelUp", fn)  -- ❌ Wrong (Subscribe is not on Events table)
> ```

| Event | Status | Ctx | Event Data | Description |
|-------|--------|-----|------------|-------------|
| `Ext.Events.SessionLoading` | ✅ | B | {} | Before save loads |
| `Ext.Events.SessionLoaded` | ✅ | B | {} | After save loads |
| `Ext.Events.ResetCompleted` | ✅ | B | {} | After reset command |
| `Ext.Events.Tick` | ✅ | B | {DeltaTime} | Every game loop (~30hz) |
| `Ext.Events.StatsLoaded` | ✅ | B | {} | After stats loaded |
| `Ext.Events.ModuleLoadStarted` | ✅ | B | {} | Before mod scripts load |
| `Ext.Events.GameStateChanged` | ✅ | B | {FromState, ToState} | State transitions |
| `Ext.Events.KeyInput` | ✅ | C | {Key, Pressed, Modifiers, Character} | Keyboard input |
| `Ext.Events.DoConsoleCommand` | ✅ | B | {Command, Prevent} | Console `!` command interception |
| `Ext.Events.LuaConsoleInput` | ✅ | B | {Input, Prevent} | Raw Lua console input interception |
| `Ext.Events.TurnStarted` | ✅ | S | {Entity, Round} | Combat turn started |
| `Ext.Events.TurnEnded` | ✅ | S | {Entity} | Combat turn ended |
| `Ext.Events.CombatStarted` | ✅ | S | {Entity} | Combat initiated |
| `Ext.Events.CombatEnded` | ✅ | S | {Entity} | Combat resolved |
| `Ext.Events.StatusApplied` | ✅ | S | {Entity, StatusId, Source} | Status effect applied |
| `Ext.Events.StatusRemoved` | ✅ | S | {Entity} | Status effect removed |
| `Ext.Events.EquipmentChanged` | ✅ | S | {Entity} | Equipment slot changed |
| `Ext.Events.LevelUp` | ✅ | S | {Entity} | Character level increased |

### Subscribing to Events

```lua
local handlerId = Ext.Events.SessionLoaded:Subscribe(function(e)
    Ext.Print("Save loaded!")
end, {Priority = 100, Once = false})

-- Unsubscribe later
Ext.Events.SessionLoaded:Unsubscribe(handlerId)
```

**Options:**
- `Priority` - Lower numbers run first (default: 100)
- `Once` - Auto-unsubscribe after first call (default: false)

### Preventable Events

Some events support the `Prevent` pattern, allowing handlers to stop the default action:

```lua
-- Intercept console commands
Ext.Events.DoConsoleCommand:Subscribe(function(e)
    Ext.Print("Command: " .. e.Command)
    if e.Command == "!secret" then
        Ext.Print("Access denied!")
        e.Prevent = true  -- Stop command execution
    end
end)

-- Intercept raw Lua input
Ext.Events.LuaConsoleInput:Subscribe(function(e)
    Ext.Print("Lua input received: " .. #e.Input .. " chars")
    -- e.Prevent = true would skip execution
end)
```

### Combat & Status Events via Osiris

Combat and status events are available through `Ext.Osiris.RegisterListener`. These fire from the Osiris scripting engine and provide comprehensive coverage for combat mechanics:

**Combat Events:**
```lua
-- Turn started (fires when a character's turn begins)
Ext.Osiris.RegisterListener("TurnStarted", 1, "after", function(charGuid)
    Ext.Print("Turn started for: " .. charGuid)
end)

-- Turn ended
Ext.Osiris.RegisterListener("TurnEnded", 1, "after", function(charGuid)
    Ext.Print("Turn ended for: " .. charGuid)
end)

-- Combat started/ended
Ext.Osiris.RegisterListener("EnteredCombat", 2, "after", function(charGuid, combatGuid)
    Ext.Print("Entered combat: " .. charGuid)
end)

Ext.Osiris.RegisterListener("LeftCombat", 2, "after", function(charGuid, combatGuid)
    Ext.Print("Left combat: " .. charGuid)
end)

-- Combat round events
Ext.Osiris.RegisterListener("CombatRoundStarted", 2, "after", function(combatGuid, round)
    Ext.Print("Round " .. round .. " started")
end)
```

**Status Events:**
```lua
-- Status applied
Ext.Osiris.RegisterListener("StatusApplied", 4, "after", function(target, status, causee, storyActionID)
    Ext.Print("Status " .. status .. " applied to " .. target)
end)

-- Status removed
Ext.Osiris.RegisterListener("StatusRemoved", 4, "after", function(target, status, causee, storyActionID)
    Ext.Print("Status " .. status .. " removed from " .. target)
end)
```

**Attack Events:**
```lua
-- Attack of opportunity
Ext.Osiris.RegisterListener("AttackedByObject", 3, "after", function(defender, attackerOwner, attacker)
    Ext.Print(defender .. " attacked by " .. attacker)
end)
```

**Rest Events:**
```lua
-- Short rest
Ext.Osiris.RegisterListener("ShortRested", 1, "after", function(charGuid)
    Ext.Print(charGuid .. " short rested")
end)

-- Long rest
Ext.Osiris.RegisterListener("LongRestStarted", 0, "after", function()
    Ext.Print("Long rest started")
end)

Ext.Osiris.RegisterListener("LongRestFinished", 0, "after", function()
    Ext.Print("Long rest finished")
end)
```

### Convenience Functions

| API | Status | Ctx | Description |
|-----|--------|-----|-------------|
| `Ext.OnNextTick(callback)` | ✅ | B | Run callback on next tick (once) |

---

## Ext.Vars

Variable persistence system with support for both mod-level storage and entity-attached data.

### PersistentVars (Mod-Level)

File-based persistence for mod data across sessions.

| API | Status | Ctx | Description |
|-----|--------|-----|-------------|
| `Mods[ModTable].PersistentVars` | ✅ | B | Per-mod persistent storage table |
| `Ext.Vars.SyncPersistentVars()` | ✅ | B | Force save all PersistentVars |
| `Ext.Vars.IsPersistentVarsLoaded()` | ✅ | B | Check if vars loaded |
| `Ext.Vars.ReloadPersistentVars()` | ✅ | B | Force reload from disk |
| `Ext.Vars.MarkDirty()` | ✅ | B | Mark for auto-save |

**Storage Location:** `~/Library/Application Support/BG3SE/persistentvars/{ModTable}.json`

**Example:**
```lua
-- In your mod's BootstrapServer.lua
Mods.MyMod.PersistentVars = Mods.MyMod.PersistentVars or {}
Mods.MyMod.PersistentVars.PlayerKills = (Mods.MyMod.PersistentVars.PlayerKills or 0) + 1
Ext.Vars.MarkDirty()  -- Will auto-save
```

### User Variables (Entity-Attached)

Attach custom data to game entities with automatic persistence.

| API | Status | Ctx | Description |
|-----|--------|-----|-------------|
| `Ext.Vars.RegisterUserVariable(name, opts)` | ✅ | B | Register a variable prototype |
| `Ext.Vars.GetEntitiesWithVariable(name)` | ✅ | B | Get all entities with a variable |
| `Ext.Vars.SyncUserVariables()` | ✅ | B | Force save user variables |
| `Ext.Vars.DirtyUserVariables([guid], [key])` | ✅ | B | Mark variables as dirty |
| `entity.Vars.VarName` | ✅ | B | Get/set entity variable |

**Registration Options:**
| Option | Default | Description |
|--------|---------|-------------|
| `Server` | true | Available on server |
| `Client` | false | Available on client |
| `Persistent` | true | Save to disk |
| `WriteableOnServer` | true | Can modify on server |
| `WriteableOnClient` | false | Can modify on client |
| `SyncToClient` | false | Sync server→client |
| `SyncToServer` | false | Sync client→server |
| `SyncOnTick` | true | Batch sync per tick |
| `SyncOnWrite` | false | Sync immediately on write |

**Storage Location:** `~/Library/Application Support/BG3SE/uservars.json`

**Example:**
```lua
-- Register variable (in BootstrapServer.lua)
Ext.Vars.RegisterUserVariable("MyMod_CustomHP", {
    Server = true,
    Persistent = true
})

-- Set variable on entity
local entity = Ext.Entity.Get(GetHostCharacter())
entity.Vars.MyMod_CustomHP = { bonus = 50, temp = 10 }

-- Read variable
local hp = entity.Vars.MyMod_CustomHP
if hp then
    Ext.Print("Bonus HP: " .. hp.bonus)
end

-- Find all entities with variable
local entities = Ext.Vars.GetEntitiesWithVariable("MyMod_CustomHP")
for _, guid in ipairs(entities) do
    Ext.Print("Entity: " .. guid)
end
```

### Mod Variables (Global Per-Mod)

Store global mod data not attached to any specific entity. Ideal for settings, counters, and mod state.

| API | Status | Ctx | Description |
|-----|--------|-----|-------------|
| `Ext.Vars.RegisterModVariable(uuid, name, opts)` | ✅ | B | Register a mod variable prototype |
| `Ext.Vars.GetModVariables(uuid)` | ✅ | B | Get mod variable proxy |
| `Ext.Vars.SyncModVariables()` | ✅ | B | Force save mod variables |
| `Ext.Vars.DirtyModVariables([uuid], [key])` | ✅ | B | Mark variables as dirty |

**Storage Location:** `~/Library/Application Support/BG3SE/modvars.json`

**Example:**
```lua
-- Get mod variables proxy (auto-creates if needed)
local mv = Ext.Vars.GetModVariables("my-mod-uuid")

-- Set values (table-like access)
mv.Counter = 42
mv.Settings = { volume = 0.8, difficulty = "Hard" }
mv.Active = true

-- Read values
Ext.Print("Counter: " .. mv.Counter)
Ext.Print("Volume: " .. mv.Settings.volume)

-- Iterate all variables
for key, value in pairs(mv) do
    Ext.Print(key .. " = " .. tostring(value))
end

-- Optional: explicit registration with options
Ext.Vars.RegisterModVariable("my-mod-uuid", "Settings", {
    Server = true,
    Persistent = true
})

-- Force save
Ext.Vars.SyncModVariables()
```

---

## Ext.Timer

Timer system for delayed and repeating function calls.

| API | Status | Ctx | Description |
|-----|--------|-----|-------------|
| `Ext.Timer.WaitFor(delay, callback, [repeat])` | ✅ | B | Create timer (delay in ms) |
| `Ext.Timer.Cancel(handle)` | ✅ | B | Cancel a timer |
| `Ext.Timer.Pause(handle)` | ✅ | B | Pause a timer |
| `Ext.Timer.Resume(handle)` | ✅ | B | Resume a paused timer |
| `Ext.Timer.IsPaused(handle)` | ✅ | B | Check if timer is paused |
| `Ext.Timer.MonotonicTime()` | ✅ | B | Get monotonic clock (ms) |

**Example:**
```lua
-- One-shot timer (5 seconds)
local handle = Ext.Timer.WaitFor(5000, function(h)
    Ext.Print("Timer fired!")
end)

-- Repeating timer (every 1 second)
local repeater = Ext.Timer.WaitFor(1000, function(h)
    Ext.Print("Tick!")
end, 1000)  -- Third arg is repeat interval

-- Cancel it later
Ext.Timer.Cancel(repeater)
```

---

## Ext.Template

Access game object templates (Character, Item, Scenery, etc.). Templates define the base properties for game objects before instantiation.

| API | Status | Ctx | Description |
|-----|--------|-----|-------------|
| `Ext.Template.Get(guid)` | ✅ | B | Cascading search for template by GUID |
| `Ext.Template.GetRootTemplate(guid)` | ✅ | B | Get template from GlobalTemplateBank |
| `Ext.Template.GetCacheTemplate(guid)` | ✅ | B | Get template from CacheTemplateManager |
| `Ext.Template.GetAllRootTemplates()` | ✅ | B | Get all templates from GlobalTemplateBank |
| `Ext.Template.GetAllCacheTemplates()` | ✅ | B | Get all templates from CacheTemplateManager |
| `Ext.Template.GetAllLocalCacheTemplates()` | ✅ | B | Get all templates from LocalCacheTemplates |
| `Ext.Template.GetAllLocalTemplates()` | ✅ | B | Get all templates from LocalTemplateManager |
| `Ext.Template.GetCount([managerType])` | ✅ | B | Get template count (Root, Cache, Local, LocalCache) |
| `Ext.Template.GetType(template)` | ✅ | B | Get template type name |
| `Ext.Template.IsReady()` | ✅ | B | Check if template manager is ready |
| `Ext.Template.LoadFridaCapture()` | ✅ | B | Load captured templates from Frida |
| `Ext.Template.HasFridaCapture()` | ✅ | B | Check if Frida capture exists |

### Template Object Properties

Templates returned from Get functions include these properties:

| Property | Type | Description |
|----------|------|-------------|
| `Guid` | string | Template GUID |
| `TemplateId` | string | Resolved template ID |
| `TemplateName` | string | Resolved template name |
| `ParentTemplateId` | string | Parent template ID (for inheritance) |
| `Handle` | number | Runtime template handle |
| `Type` | string | Template type (Character, Item, Scenery, etc.) |
| `RawType` | string | Raw type string from game |
| `_ptr` | userdata | Raw pointer (for debugging) |

### Example

```lua
-- Get a template by GUID
local template = Ext.Template.Get("some-guid-here")
if template then
    _P("Template: " .. (template.TemplateName or "unknown"))
    _P("Type: " .. template.Type)
end

-- List all root templates
local templates = Ext.Template.GetAllRootTemplates()
_P("Found " .. #templates .. " root templates")

for i, t in ipairs(templates) do
    if i <= 5 then
        _P(t.Guid .. " -> " .. (t.TemplateName or "?"))
    end
end
```

**Note:** Template access requires Frida capture or runtime template manager discovery. Use `Ext.Template.IsReady()` to check availability.

---

## Ext.Resource

Access game resources (Visual, Sound, Material, Texture, Dialog, etc.). Resources are non-GUID assets loaded by the ResourceManager.

| API | Status | Ctx | Description |
|-----|--------|-----|-------------|
| `Ext.Resource.Get(resourceId, type)` | ✅ | B | Get a resource by ID and type |
| `Ext.Resource.GetAll(type)` | ✅ | B | Get all resources of a type |
| `Ext.Resource.GetTypes()` | ✅ | B | Get list of supported resource types |
| `Ext.Resource.GetCount(type)` | ✅ | B | Get count of resources for a type |
| `Ext.Resource.IsReady()` | ✅ | B | Check if ResourceManager is available |

### Supported Resource Types (34 types)

```
Visual, VisualSet, Animation, AnimationBlueprint, AnimationSet,
Skeleton, Physics, Material, Texture, Script, Effect, Sound,
MaterialSet, MeshProxy, Terrain, IKRig, Dialog, VoiceBark,
TileSet, MaterialPreset, DiffusionProfile, CharacterVisual,
Lighting, Atmosphere, LevelMap, ColorPreset, ColorList,
TextureMerge, FCurve, BlendSpace, Timeline, ClothCollider,
ClothAsset, SkeletonMirrorTable
```

### Resource Object Properties

| Property | Type | Description |
|----------|------|-------------|
| `Type` | string | Resource type name |
| `ResourceId` | string | Resource ID (resolved from FixedString) |
| `_id` | number | Raw FixedString ID |
| `_ptr` | userdata | Raw pointer (for debugging) |
| `_ptrHex` | string | Pointer as hex string |

### Example

```lua
-- Check if ResourceManager is ready
if Ext.Resource.IsReady() then
    -- Get all Visual resources
    local visuals = Ext.Resource.GetAll("Visual")
    _P("Found " .. #visuals .. " Visual resources")

    -- Get supported types
    local types = Ext.Resource.GetTypes()
    _P("Supported types: " .. table.concat(types, ", "))

    -- Get count for a specific type
    local count = Ext.Resource.GetCount("Material")
    _P("Material resources: " .. count)
end
```

---

## Ext.StaticData

Access immutable game data (Feats, Races, Backgrounds, Origins, etc.). StaticData entries are loaded from game files and cannot be modified at runtime.

| API | Status | Ctx | Description |
|-----|--------|-----|-------------|
| `Ext.StaticData.GetAll(type)` | ✅ | B | Get all entries of a type as array |
| `Ext.StaticData.Get(type, guid)` | ✅ | B | Get single entry by GUID string |
| `Ext.StaticData.GetCount(type)` | ✅ | B | Get count of entries for a type |
| `Ext.StaticData.GetTypes()` | ✅ | B | Get list of available types |
| `Ext.StaticData.IsReady()` | ✅ | B | Check if StaticData is available |

### Supported Types (9 types)

| Type | Description |
|------|-------------|
| `Feat` | Character feats and abilities |
| `Race` | Playable races |
| `Background` | Character backgrounds |
| `Origin` | Origin characters |
| `God` | Deity options |
| `Class` | Character classes |
| `Progression` | Level progression data |
| `ActionResource` | Action resources (spell slots, etc.) |
| `FeatDescription` | Feat description text |

### StaticData Entry Properties

| Property | Type | Description |
|----------|------|-------------|
| `ResourceUUID` | string | Entry GUID |
| `Name` | string | Internal name (if available) |
| `DisplayName` | string | Display name (if available) |
| `Type` | string | Static data type |
| `_ptr` | userdata | Raw pointer (for debugging) |

### Example

```lua
-- List all feats
local feats = Ext.StaticData.GetAll("Feat")
_P("Found " .. #feats .. " feats")

for i, feat in ipairs(feats) do
    if i <= 5 then
        _P(feat.ResourceUUID .. " -> " .. (feat.Name or "unnamed"))
    end
end

-- Get a specific race by GUID
local human = Ext.StaticData.Get("Race", "0eb594cb-8820-4be6-a58d-8be7a1a98fba")
if human then
    _P("Found race: " .. (human.DisplayName or human.Name or "unknown"))
end

-- Check counts
for _, type in ipairs(Ext.StaticData.GetTypes()) do
    local count = Ext.StaticData.GetCount(type)
    _P(type .. ": " .. count .. " entries")
end
```

**Note:** StaticData uses ForceCapture to populate managers at runtime. If `IsReady()` returns false, static data may not be fully loaded yet.

---

## Ext.Osiris

Interface to the Osiris scripting engine. **Server-only** - Osiris runs on the server context only.

| API | Status | Ctx | Description |
|-----|--------|-----|-------------|
| `Ext.Osiris.RegisterListener(event, arity, timing, callback)` | ✅ | S | Register Osiris event callback; returns a subscription id |
| `Ext.Osiris.UnregisterListener(id)` | ✅ | S | Remove a subscription; `true` if it was live, `false` otherwise |
| `Ext.Osiris.NewQuery(name, signature, handler)` | 🍎 | S | Register custom query (returns values) |
| `Ext.Osiris.NewCall(name, signature, handler)` | 🍎 | S | Register custom call (no return) |
| `Ext.Osiris.NewEvent(name, signature)` | 🍎 | S | Register custom event |
| `Ext.Osiris.RaiseEvent(name, ...)` | 🍎 | S | Raise a custom event to dispatch to listeners |
| `Ext.Osiris.GetCustomFunctions()` | 🍎 | S | Get table of all registered custom functions (debug) |

🍎 = **macOS-port-only.** Upstream's `Ext.Osiris` is exactly `RegisterListener`
and `UnregisterListener` — nothing else. Custom Osiris functions (the DOS2
extender's `NewCall`/`NewQuery`/`NewEvent`) do not exist in BG3SE at all, so no
mod written against upstream calls them; they are kept here for the port's own
tooling and for mods authored against this port.

**Timing values:** `"before"`, `"after"`, `"beforeDelete"` or `"afterDelete"`.
Anything else raises `Hook type must be 'before', 'beforeDelete', 'after' or
'afterDelete'` (upstream `LuaServer.cpp`). Note that only engine *events*
currently dispatch on this port: database inserts/deletes, PROC calls and QRY
calls are not yet hooked, so `beforeDelete`/`afterDelete` (and `before`/`after`
on a `DB_*`/`PROC_*`/`QRY_*` name) register without error but never fire.

**Example - Event Listener:**
```lua
Ext.Osiris.RegisterListener("AutomatedDialogStarted", 2, "after", function(dialog, instanceId)
    Ext.Print("Dialog started: " .. tostring(dialog))
end)
```

**Example - Custom Query:**
```lua
-- Register a custom query with IN and OUT parameters
Ext.Osiris.NewQuery("MyMod_Add", "[in](INTEGER)_A,[in](INTEGER)_B,[out](INTEGER)_Sum",
    function(a, b)
        return a + b
    end)

-- Call via Osi namespace
local result = Osi.MyMod_Add(10, 20)  -- Returns 30
```

**Example - Custom Call:**
```lua
-- Register a custom call (no return value)
Ext.Osiris.NewCall("MyMod_Log", "(STRING)_Message",
    function(msg)
        Ext.Print("Custom: " .. msg)
    end)

-- Call it
Osi.MyMod_Log("Hello from Lua!")
```

**Signature Format:**
- `[in]` - Input parameter (default if omitted)
- `[out]` - Output parameter (for queries)
- Types: `INTEGER`, `INTEGER64`, `REAL`, `STRING`, `GUIDSTRING`
- Example: `"[in](GUIDSTRING)_Target,[out](INTEGER)_Health"`

**Example - Custom Event with RaiseEvent:**
```lua
-- Register a custom event
Ext.Osiris.NewEvent("MyMod_ItemCollected", "(GUIDSTRING)_Item,(GUIDSTRING)_Collector")

-- Register a listener for the event
Ext.Osiris.RegisterListener("MyMod_ItemCollected", 2, "after", function(item, collector)
    Ext.Print("Item " .. item .. " collected by " .. collector)
end)

-- Raise the event from Lua (dispatches to all listeners)
local itemGuid = "ITEM_Gold_123"
local playerGuid = "S_PLA_Gale_..."
local numListenersCalled = Ext.Osiris.RaiseEvent("MyMod_ItemCollected", itemGuid, playerGuid)
```

**Example - Debug Custom Functions:**
```lua
-- List all registered custom functions
for name, info in pairs(Ext.Osiris.GetCustomFunctions()) do
    Ext.Print(name .. " (" .. info.Type .. ") - " .. info.Arity .. " params")
end
```

---

## Ext.Input

Keyboard input capture and injection (macOS CGEventTap). **Client-only** - input handling runs in client context.

| API | Status | Ctx | Description |
|-----|--------|-----|-------------|
| `Ext.Input.RegisterHotkey(key, callback)` | ✅ | C | Register hotkey handler |
| `Ext.Input.UnregisterHotkey(key)` | ✅ | C | Remove hotkey handler |
| `Ext.Input.InjectKey(key, pressed)` | ✅ | C | Simulate key press/release |
| `Ext.Input.IsKeyPressed(key)` | ✅ | C | Check current key state |

---

## Ext.Math

Vector and matrix operations.

### Vector Types

| Type | Description |
|------|-------------|
| `vec3` | 3D vector (x, y, z) |
| `vec4` | 4D vector (x, y, z, w) |
| `mat3` | 3x3 matrix |
| `mat4` | 4x4 matrix |

### Operations

```lua
local v1 = Ext.Math.vec3(1, 2, 3)
local v2 = Ext.Math.vec3(4, 5, 6)
local sum = v1 + v2
local dot = Ext.Math.Dot(v1, v2)
local cross = Ext.Math.Cross(v1, v2)
local normalized = Ext.Math.Normalize(v1)
```

---

## Ext.Memory (Development)

Low-level memory access for debugging and offset discovery.

| API | Status | Ctx | Description |
|-----|--------|-----|-------------|
| `Ext.Memory.Read(addr, size)` | ✅ | B | Read bytes as hex string |
| `Ext.Memory.ReadString(addr, maxLen)` | ✅ | B | Read null-terminated string |
| `Ext.Memory.Search(pattern, start, size)` | ✅ | B | Search for byte pattern |
| `Ext.Memory.GetModuleBase(name)` | ✅ | B | Get base address of loaded module |

---

## Ext.Debug (Development)

Safe memory introspection for offset discovery.

| API | Status | Ctx | Description |
|-----|--------|-----|-------------|
| `Ext.Debug.ReadPtr(addr)` | ✅ | B | Read pointer (safe) |
| `Ext.Debug.ReadU32(addr)` | ✅ | B | Read uint32 |
| `Ext.Debug.ReadI32(addr)` | ✅ | B | Read int32 |
| `Ext.Debug.ReadU64(addr)` | ✅ | B | Read uint64 |
| `Ext.Debug.ReadFloat(addr)` | ✅ | B | Read float |
| `Ext.Debug.ReadString(addr, max)` | ✅ | B | Read C string |
| `Ext.Debug.ProbeStruct(base, start, end, stride)` | ✅ | B | Bulk offset discovery |
| `Ext.Debug.HexDump(addr, size)` | ✅ | B | Hex dump memory |
| `Ext.Debug.FindArrayPattern(base, range)` | ✅ | B | Find array patterns |

---

## Global Debug Functions

Available in the console for quick debugging:

| Function | Description | Example |
|----------|-------------|---------|
| `_P(...)` | Print (alias for Ext.Print) | `_P("hello")` |
| `_D(obj)` | Dump object as JSON | `_D(Ext.Stats.Get("WPN_Longsword"))` |
| `_DS(obj)` | Dump shallow (depth=1) | `_DS(someTable)` |
| `_H(n)` | Format as hex | `_H(255)` → "0xff" |
| `_PTR(base, off)` | Pointer arithmetic | `_PTR(base, 0x10)` |
| `_PE(...)` | Print error | `_PE("failed!")` |
| `GetHostCharacter()` | Returns main player GUID | |

---

## Osi Namespace

Every Osiris symbol the running story defines, resolved dynamically through
`Osi`'s `__index` — there are no hand-written per-function bindings. **Server-only**
- Osiris runs on the server context.

| Form | Ctx | Behaviour |
|------|-----|-----------|
| `Osi.<Call>(...)` | S | Dispatches the engine call; returns nothing |
| `Osi.<Query>(...)` | S | Returns one value per OUT param (all `nil` if the query fails); a query with no OUT params returns a **boolean** |
| `Osi.<Proc>(...)` / `Osi.<Event>(...)` | S | Story symbols with no dispatch id: inserted into their RETE node |
| `Osi.DB_<Name>:Get(...)` | S | Rows of the database matching the filter (`nil` = wildcard) |
| `Osi.DB_<Name>:Delete(...)` | S | Deletes **every** matching row (`nil` = wildcard) |
| `Osi.DB_<Name>(...)` | S | Inserts a tuple |
| `Osi.<Unknown>` | S | `nil`, once the story is loaded — use it for feature detection |

Overloads are selected by the number of arguments you pass, matching upstream:
`Osi.MakePlayer(char)` and `Osi.MakePlayer(char, owner, canReassign)` are
different engine functions. Passing a count no overload accepts raises
`No function named 'X' exists that can be called with N parameters.`

Argument types are checked against the story's declared parameter types:
a number where a `CHARACTER`/`STRING` is declared raises `String expected for
argument N, got number`, and vice versa (upstream `ValueHelpers.inl`).

---

## Console Commands

Register custom console commands (invoked with `!` prefix):

```lua
Ext.RegisterConsoleCommand("mystats", function(cmd, statName)
    local stat = Ext.Stats.Get(statName)
    if stat then
        stat:Dump()
    else
        Ext.Print("Stat not found: " .. tostring(statName))
    end
end)
```

Then use in console:
```
!mystats WPN_Longsword
```

### Built-in Commands

| Command | Description |
|---------|-------------|
| `!help` | List all registered commands |
| `!probe <addr> <range>` | Probe memory structure |
| `!dumpstat <name>` | Dump stat object |
| `!hexdump <addr> <size>` | Hex dump memory |
| `!types` | List registered component types |
