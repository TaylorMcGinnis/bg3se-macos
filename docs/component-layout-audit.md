# Component layout audit vs upstream

Generated 2026-09-13 by comparing `src/entity/component_offsets.h` against
upstream's `DEFINE_COMPONENT` declarations and struct bodies in
`BG3Extender/GameDefinitions/Components/`.

Prompted by two stubs that hid real data from mods for months:

- `esv::DisplayNameListComponent` — `Names` was an opaque array, so the
  component the server composes display names from was unreadable.
- `eoc::DisplayNameComponent` — only flat handles were exposed, so the nested
  `NameKey.Handle.Handle` path that Norbyte's own docs use returned nil.
  VolitionCabinet, a library several mods build on, raised
  "attempt to index a nil value (field 'NameKey')" four times a frame because of
  it.

## Lua-name mismatches: mostly cosmetic, but the fallback has holes

99 hand-written layouts carry a `shortName` differing from upstream's name (ours
`AttributeFlagsComponent`, upstream `AttributeFlags`). Entity lookup falls back
through the generated upstream name table, so **where the table has an entry,
both names resolve** — verified on `CharacterCreationStats`, `AttributeFlags`,
`BodyType`, `Voice`, `ApprovalRatings`, `CharacterCreationAppearance`.

**An earlier version of this document called all 99 cosmetic. That was wrong**,
generalised from six samples that happened to be covered. The fallback is only
as good as the table, and the table had two holes:

1. **284 components were missing — over a quarter of the total.** The generator
   scanned for `DEFINE_COMPONENT(name, "class")`, but two macros declare
   components without a quoted class string, so it saw neither:

   | Macro | Shape | Count |
   |---|---|---|
   | `DEFN_BOOST` (`Base/Base.h:84`) | builds the name by token-pasting: `#name "Boost"`, `"eoc::" #name "BoostComponent"` | 103 |
   | `DEFINE_TAG_COMPONENT` (`:64`, `:74`) | three bare identifiers: name is the 3rd arg, class is `ns::name` | 181 |

   `e.WeaponDamageBoost` was nil while `e.WeaponDamageBoostComponent` worked.
   Both are fixed in the **generator**, deriving the strings exactly as the
   macros expand them, so the fix survives regeneration: 783 → **1067** entries.
   Verified live — `ServerStatusBoostsProcessed` 14912, `SpellCastCanBeTargeted`
   24629, `AnubisEnabled` 1656, all previously 0.
2. **`Ext.Entity.GetAllEntitiesWithComponent` never consulted the table at all.**
   It resolved through the class-name-keyed registry plus two hardcoded aliases,
   so `GetAllEntitiesWithComponent("AbilityBoost")` returned an empty table while
   the full class name found 254 entities — with no error to explain it. It now
   falls back through the upstream table and this port's short names.

Do not mass-rename layouts. Do check that a name actually resolves before
concluding a mismatch is harmless.

## Opaque arrays: 45, and these ARE the problem

A `FIELD_TYPE_DYNAMIC_ARRAY` with no element type reads as an empty proxy —
`#arr` works, `arr[i]` is nil — so the contents are invisible to mods and there
is no error to notice. This is exactly what hid `DisplayNameList.Names`.

Fixed so far (element type taken from upstream's declaration; sizes fixed and
certain, or a struct layout already verified live):

| Component | Field | Upstream | Element |
|---|---|---|---|
| `eoc::VoiceTagComponent` | `Tags` | `Array<Guid>` | GUID/16 |
| `eoc::god::TagComponent` | `Tags` | `Array<Guid>` | GUID/16 |
| `eoc::combat::IsThreatenedComponent` | `ThreatenedBy` | `Array<EntityHandle>` | HANDLE/8 |
| `eoc::ObjectInteractionComponent` | `Interactions` | `Array<EntityHandle>` | HANDLE/8 |
| `eoc::lock::LockComponent` | `field_18` | `Array<Guid>` | GUID/16 |
| `esv::inventory::ShapeshiftEquipmentHistoryComponent` | `History` | `Array<Guid>` | GUID/16 |
| `eoc::action::ActionUseConditionsComponent` | `Conditions` | `Array<int32_t>` | INT32/4 |
| `eoc::TurnOrderComponent` | `TurnOrderIndices`, `TurnOrderIndices2` | `Array<uint64_t>` | UINT64/8 (element type added) |
| `eoc::spell::CCPrepareSpellComponent` | `Spells` | `Array<SpellMetaId>` | STRUCT/0x30 **verified live** |
| `eoc::spell::PlayerPrepareSpellComponent` | `Spells` | `Array<SpellMetaId>` | STRUCT/0x30 **verified live** |
| `eoc::UseBoostsComponent` | `Boosts` | `Array<BoostDescription>` | STRUCT/0x0c **verified live** |
| `esv::IconListComponent` | `Icons` | `Array<IconInfo>` | STRUCT/0x08 **verified live** |
| `ls::animation::TemplateAnimationSetOverrideComponent` | `Overrides` | `Array<AnimationWaterfallElement>` | STRUCT/0x0c **verified live** |
| `esv::ActivationGroupContainerComponent` | `Groups` | `Array<ActivationGroupData>` | STRUCT/0x08 **verified live** |
| `ls::animation::DynamicAnimationTagsComponent` | `Tags` | `Array<AnimationTag>` | STRUCT/0x18 **verified live** |
| `ls::trigger::IsInsideOfComponent` | `Triggers` (+ `InsideOf`) | `Array<Guid>` | GUID/16 **verified live** |
| `eoc::spell::ContainerComponent` | `Spells` | `Array<SpellMeta>` | STRUCT/**0x60** **verified live** (was 80 — a bug) |
| `eoc::spell::AddedSpellsComponent` | `Spells` | `Array<SpellMeta>` | STRUCT/0x60 **verified live** |

Verified live by scanning every entity carrying the component
(`Ext.Entity.GetAllEntitiesWithComponent`, far better than checking party
members — the first two attempts found nothing because boosts live on items):

- `SpellMetaId` arrays: 5 entries, `[1].OriginatorPrototype = "Target_Sanctuary"`
- `UseBoosts.Boosts`: `[1].Boost = "Advantage"`, `.Params = "AttackRoll"`
- `IconList.Icons`: `[1].Icon = "Item_CONT_HAG_ThornyBush"`, `.field_4 = 4`
- `TemplateAnimationSetOverride.Overrides`: `[1].Type = "TemplateOverride"`
- `ActivationGroupContainer.Groups`: `[1].field_4 = "ParentPlatform"`
- `DynamicAnimationTags.Tags`: 4 and 3 entries on two entities, `[1].Tag` a real GUID

`BoostDescription` (three FixedStrings, 0x0c) and `IconInfo` (FixedString +
uint32, 0x08) have strides that are *derived* rather than hex-dumped, which is
acceptable only because every member is 4 bytes — there is no type here whose
size varies by toolchain, which is the hazard that made `TranslatedString`,
`STDString` and `std::optional` untrustworthy. Both are now confirmed live
anyway.

The remaining typed arrays read their headers correctly but had no populated
instance, so their element decoding is inferred, not observed.

### Caught by this audit: `eoc::UseComponent` array offsets are WRONG

Typing `Use.Boosts` as `Array<BoostDescription>` produced element proxies whose
buffer pointer was `0x1` — the invalid-array sentinel — meaning the `Array`
header is not at the offset this layout claims. The typing was reverted and the
component left opaque **on purpose**: an untyped array yields nothing, while a
typed one over a bad offset yields plausible-looking garbage, which is worse.
`Use.Requirements`, `Use.Boosts`, `BoostsOnEquipMainHand` and
`BoostsOnEquipOffHand` all need their offsets established live before typing.
Note `eoc::UseBoostsComponent.Boosts` is a *different* component and reads
correctly.

### Still to do, needing a MEASURED stride

Each needs a struct layout plus the element stride established live (expose the
raw buffer and count, hex dump, find where the next element's header repeats,
confirm on two entities):
`LevelUpData` (contains a `std::array` and a nested `Array`),
`SurfacePathInfluence` (leading enum of unknown width; no entity in the test
save carries `SurfacePathInfluences` at all, so it cannot be measured there),
`BaseWeaponDamage` (contains `RollDefinition`, size unknown),
`stats::Requirement`, `SpellMeta`, `State` (shapeshift; large and
`std::optional`-heavy).

Structs whose members are ALL fixed-size PODs can have their stride derived
rather than measured — there is no type whose size varies by toolchain, which is
the hazard behind `TranslatedString`, `STDString` and `std::optional`. That is
how `BoostDescription` (0x0c), `IconInfo` (0x08), `AnimationWaterfallElement`
(0x0c), `ActivationGroupData` (0x08) and `AnimationTag` (0x18) were done, and
all five were then confirmed live anyway. Anything containing an enum, a
`std::optional`, a `std::array` or another struct of unknown size must be
measured.

## Layouts that do NOT match upstream

Widening the search past `DEFINE_COMPONENT` revealed a worse class of problem
than untyped arrays: some hand-written layouts describe components upstream does
not have, or describe the wrong members.

**Corrected: `eoc::DifficultyCheckComponent` was reading the wrong memory.**
Upstream is `HashMap<AbilityId,uint32> AbilityDC` (legacy `field_0`), `int32
SpellSaveDCBoost` (legacy `field_40`), `int32 WeaponActionDC` (legacy
`field_44`). A HashMap is 0x40 here, so the ints belong at 0x40/0x44 — which the
recorded 0x48 component size confirms. The old layout had them at 0x20/0x24,
reading the middle of the map as integers, and described the map's first two
words as dynamic arrays. Verified after the fix: `WeaponActionDC` reads 3 and 8
on real entities. `AbilityDC` is deliberately left undescribed — the key is an
`AbilityId` enum of unestablished width, and a wrongly-typed map is worse than
an absent one.

**Corrected: `SpellMeta` is 0x60, not 80.** `eoc::spell::ContainerComponent.Spells`
carried `ELEM_TYPE_SPELL_META, 80`, and `ELEM_TYPE_SPELL_META`'s own comment
asserted "80 bytes" — neither had been checked on this build. Measured from a
16-element `AddedSpells` buffer: the element header repeats at **+0x60**. At 80
bytes every element after the first was read misaligned, silently returning
plausible-looking wrong data from a component mods use constantly.

`SpellMeta` now has a real layout (`SpellId` at 0x00 using the verified
`SpellMetaId`, `BoostHandle` at 0x30) instead of decoding to an opaque `__ptr`
table. Its remaining enum/Guid members are left undescribed — upstream's legacy
`field_29` name contradicts the current order, so their offsets are not
established. Verified live: five consecutive elements read
`Projectile_Jump, Target_Dip, Shout_Hide, Target_Shove, Throw_Throw`, matching
the order `SpellBookPrepares.PreparedSpells` reports for the same character from
a different component.

**Corrected: two boost layouts read past the end of their component.** Found by
sweeping every layout for fields whose offset+width exceeds the recorded
`componentSize` — a mechanical check worth re-running after any layout change:

- `eoc::IgnoreDamageThresholdMinBoostComponent` had `Amount` as a `uint32` at
  offset **5** — unaligned, and past the end of a 4-byte component. Upstream is
  `DamageType(1) + bool All(1) + uint16 Amount(2)` = 4, matching the size exactly.
- `eoc::RedirectDamageBoostComponent` spaced its two damage types 4 bytes apart,
  putting `DamageType2` past the end of an 8-byte component. Upstream is
  `int32 Amount + DamageType1(1) + DamageType2(1) + bool(1)` = 8, and upstream's
  legacy name `field_6` confirms the bool's offset independently.

**`DamageType` is a ONE-byte enum.** Both corrections turn on this, proved by the
size arithmetic above. **12 fields across these layouts still type it as
int32/uint32** (`component_offsets.h`, search `"DamageType`). Reading a 1-byte
enum as a 32-bit value pulls in three neighbouring bytes, so those are all
returning wrong values — but each needs its own component's size arithmetic
before being changed, since narrowing a field can imply different offsets for
what follows. Only the two proven above were touched.

**Left alone, flagged as suspect** (guessing would make them worse):

| Ours | Upstream | Problem |
|---|---|---|
| `esv::SummonContainerComponent.Summons` | `eoc::summon::ContainerComponent` — `HashMap` + two `HashSet`s | different class, no such array |
| `esv::InterruptDataComponent.Data` | `esv::spell_cast::InterruptDataComponent` — no `Data` member | different component |
| `ls::animation::RemoveAnimationSets…AnimationSets` | `HashSet<FixedString>` | **not an array** — must not be typed as one |

**No upstream counterpart at all** (so nothing to check them against; presumably
derived from Ghidra, and the field names are this port's own):
`esv::ChasmDataComponent`, `esv::ConstellationHelperComponent`,
`esv::InventoryOwnerComponent`, `ls::uuid::ToHandleMappingComponent`,
`eoc::spell::BookCooldownsComponent`.

### Still to do, element type unknown

Upstream does not declare these where a header search could find them, so they
need a wider search or disassembly: `esv::ChasmDataComponent.Data`,
`esv::ConstellationHelperComponent.Data`, `esv::InterruptDataComponent.Data`,
`esv::InventoryOwnerComponent.Inventories`, both ping singletons' `Requests`,
`esv::SummonContainerComponent.Summons`, `ls::trigger::IsInsideOfComponent.Triggers`,
`ls::uuid::ToHandleMappingComponent.Mappings`, the two animation request
components, `eoc::spell::BookCooldownsComponent.Cooldowns`,
`eoc::WeaponDamageResistanceBoostComponent.DamageTypes`,
`eoc::DifficultyCheckComponent.Abilities`/`field_30`,
`eoc::ACOverrideFormulaBoostComponent.AddAbilityModifiers`.

### Not arrays -- do not type as such

`esv::BaseDataComponent.Resistances` is a fixed `std::array<std::array<...,7>,2>`;
`esv::CustomStatsComponent.Stats` is a `LegacyMap<FixedString,int>`.

## Mechanical checks worth re-running

Both found real defects and cost nothing:

1. **Fields past the end of their component** — for each property, does
   `offset + width` exceed `componentSize`? This found both boost layouts above.
   Note that nested struct layouts with `componentSize = 0` produce noise, and
   that a layout sharing a property array with another (as `SpellMetaId` shares
   `SpellId`'s) will false-positive unless `propertyCount` is honoured.
2. **Element size vs element type** — does a typed array's `elemSize` match the
   width its `ELEM_TYPE_*` implies, and for struct elements, the referenced
   layout's `componentSize`? Currently clean.

## How to measure a struct element's stride

Expose the array's raw buffer and count read-only (`buf @0x00`, `size @0x0c`),
hex dump it, and find where the next element's header repeats. Confirm on two
different entities. This is how `esv::DisplayName`'s 0x50 stride was established
after the header's legacy names implied a different `TranslatedString` size than
this build uses.

## Generated layouts: offsets were computed wrong (2026-09-13)

`generated_property_defs.h` is produced by `tools/generate_layouts.py` from the
Windows headers' field lists. The field *names and order* come from upstream, but
the *offsets* were computed here -- and `calculate_offsets` computed them wrong.

`parse_type` returns `(None, size)` for a field Lua cannot surface: a pointer,
a container, a string. The `None` means "do not expose", the size means "but it
still occupies this much space". `calculate_offsets` read the `None` and
`continue`d **without advancing the cursor**, so every field after a skipped one
sat too low by exactly that field's width.

That is not a corner case. `esv::Projectile` begins with a vtable pointer, so all
48 of its fields were off by 8. It is also the already-known
`eoc::DifficultyCheckComponent` bug -- 0x20/0x24 where the truth was 0x40/0x44,
one skipped 32-byte field -- found by hand earlier in this audit without
recognising it as a generator defect rather than a one-off.

Measured across the 293 layouts the generator emitted: **79 had wrong offsets,
affecting 357 individual fields.** Every one of them returned a plausible number
read from the wrong address, and none of them ever threw.

Three further defects in the same function:

- `STDString` was sized 32 and `TranslatedString` 40, copied from Windows. This
  build's are **16** (`src/core/stdstring.h`, verified live) and **0x20**
  (measured in `eoc::DisplayNameComponent`).
- Alignment was `min(size, 8)`, which over-aligns `glm::vec3`: 12 bytes with
  4-byte alignment, not 8. 30 fields moved as a result of fixing this.
- Any qualified type whose name contained `Type`, `Id` or `Flags` was guessed at
  4 bytes. Enums here are frequently 1 byte -- `DamageType` is -- so the guess
  shifted everything after it.

### Two oracles now gate every emitted layout

Rather than replace one set of unverified offsets with another, the generator now
has to *corroborate* a layout or drop it. A missing layout reads as nil, which is
loud; a wrong one reads as a number, which is silent.

1. **Self-naming, per field.** The Windows headers name unidentified fields after
   their own offset -- `field_1B0` sits at 0x1B0. Each is therefore its own
   assertion about the packing. The old generator scored **173/380**; it now
   scores **219/219**.
2. **The struct total**, for layouts with nothing self-named: the computed size
   must equal the size this binary reports.

Self-naming outranks the total, because a total can legitimately differ when the
Windows header omits trailing members -- `eoc::relation::FactionComponent`'s three
self-named fields all land exactly while the listed fields sum to 0x28 against a
real 0x30. Requiring both would have discarded a layout that was already correct.

A uniform non-zero delta across every self-named field means an unmodelled
base-class prefix (`ecl::character_creation::DefinitionStateComponent` is +8,
`DummyDefinitionComponent` +0x10). That is distinguishable from a genuine
Windows/ARM64 divergence, which would shift each field by a *different* amount as
the differences accumulate -- so a uniform delta is applied as a prefix, but only
where the total then also lands on the reported size.

Container widths were not guessed either: fitting every computable struct against
the size oracle scored 287/333 exact at `Array=16 / HashSet=48 / HashMap=64`, and
lower at every other combination.

Unknown-width types now **truncate** the layout instead of being skipped. Fields
before the unknown are still correct, because a field's offset depends only on
what precedes it; nothing after it is knowable.

### Result: 293 layouts -> 213

357 wrong fields are gone. 310 correct fields kept their offsets, 30 moved for the
`vec3` alignment fix. The hand-verified layouts in `component_offsets.h` are
untouched and still win on lookup, so none of the earlier hand-verification was
disturbed. 56 components lost coverage they should not be assumed to have had:

| Component | Why it was dropped |
| --- | --- |
| `ecl::GameCameraBehavior` | self-named offsets disagree |
| `ecl::TLPreviewDummy` | no Lua-visible field before the first unknown type |
| `ecl::camera::SelectorModeComponent` | no ARM64 size, nothing self-named |
| `ecl::character_creation::DefinitionStateComponent` | uniform base-class prefix, unconfirmed |
| `ecl::character_creation::DummyDefinitionComponent` | uniform base-class prefix, unconfirmed |
| `ecl::photo_mode::CameraSavedTransformComponent` | no Lua-visible field before the first unknown type |
| `ecl::photo_mode::RequestedSingletonComponent` | no ARM64 size, nothing self-named |
| `eoc::BlockAbilityModifierFromACComponent` | no ARM64 size, nothing self-named |
| `eoc::CanInteractComponent` | no Lua-visible field before the first unknown type |
| `eoc::CustomIconComponent` | no Lua-visible field before the first unknown type |
| `eoc::FloatingComponent` | uniform base-class prefix, unconfirmed |
| `eoc::GameplayLightComponent` | uniform base-class prefix, unconfirmed |
| `eoc::ObjectSizeComponent` | computed size != ARM64 size |
| `eoc::SteeringComponent` | self-named offsets disagree |
| `eoc::camp::ChestComponent` | uniform base-class prefix, unconfirmed |
| `eoc::character_creation::ChangeAppearanceDefinitionComponent` | no Lua-visible field before the first unknown type |
| `eoc::character_creation::CharacterDefinitionComponent` | no Lua-visible field before the first unknown type |
| `eoc::character_creation::DefinitionCommonComponent` | uniform base-class prefix, unconfirmed |
| `eoc::character_creation::FullRespecDefinitionComponent` | no Lua-visible field before the first unknown type |
| `eoc::character_creation::LevelUpDefinitionComponent` | no Lua-visible field before the first unknown type |
| `eoc::dialog::StateComponent` | self-named offsets disagree |
| `eoc::hit::AttackerComponent` | no ARM64 size, nothing self-named |
| `eoc::hit::ThrownObjectComponent` | no ARM64 size, nothing self-named |
| `eoc::hit::WeaponComponent` | no ARM64 size, nothing self-named |
| `eoc::interrupt::ActionStateComponent` | no Lua-visible field before the first unknown type |
| `eoc::party::CompositionComponent` | computed size != ARM64 size |
| `eoc::party::MemberComponent` | computed size != ARM64 size |
| `eoc::progression::MetaComponent` | uniform base-class prefix, unconfirmed |
| `eoc::projectile::SourceInfoComponent` | no Lua-visible field before the first unknown type |
| `eoc::rest::LongRestState` | no Lua-visible field before the first unknown type |
| `eoc::ruleset::RulesetComponent` | self-named offset precedes computed offset |
| `eoc::shapeshift::ReplicatedChangesComponent` | no Lua-visible field before the first unknown type |
| `eoc::spatial_grid::DataComponent` | no Lua-visible field before the first unknown type |
| `eoc::spell_cast::AnimationInfoComponent` | self-named offsets disagree |
| `eoc::spell_cast::MovementComponent` | computed size != ARM64 size |
| `eoc::spell_cast::SyncTargetingComponent` | no Lua-visible field before the first unknown type |
| `eoc::unsheath::StateComponent` | self-named offsets disagree |
| `esv::AnubisExecutorComponent` | all_fields_out_of_bounds |
| `esv::BreadcrumbComponent` | no Lua-visible field before the first unknown type |
| `esv::combat::CombatGroupMappingComponent` | no ARM64 size, nothing self-named |
| `esv::combat::FleeRequestComponent` | no ARM64 size, nothing self-named |
| `esv::death::DelayedDeathComponent` | no Lua-visible field before the first unknown type |
| `esv::death::StateComponent` | no ARM64 size, nothing self-named |
| `esv::sight::AggregatedGameplayLightDataComponent` | no ARM64 size, nothing self-named |
| `esv::sight::AiGridViewshedComponent` | no ARM64 size, nothing self-named |
| `esv::spell_cast::CastResponsibleComponent` | no ARM64 size, nothing self-named |
| `esv::spell_cast::InterruptDataComponent` | no ARM64 size, nothing self-named |
| `esv::spell_cast::MovementComponent` | no ARM64 size, nothing self-named |
| `esv::spell_cast::StateComponent` | no Lua-visible field before the first unknown type |
| `esv::stats::proficiency::BaseProficiencyComponent` | no Lua-visible field before the first unknown type |
| `esv::status::CauseComponent` | computed size != ARM64 size |
| `esv::status::aura::RemovedStatusAuraEffectEventOneFrameComponent` | no ARM64 size, nothing self-named |
| `ls::DecalComponent` | all_fields_out_of_bounds |
| `ls::EffectComponent` | prefix runs past end of component |
| `ls::VisualChangeRequestOneFrameComponent` | no ARM64 size, nothing self-named |
| `ls::trigger::AreaComponent` | no Lua-visible field before the first unknown type |

Of these, the 11 dropped for a *proven* mismatch were actively returning wrong
values, `eoc::party::MemberComponent` and `eoc::relation::FactionComponent` among
them. The rest are merely uncorroborated, and are the worklist for hand
verification -- the same treatment `component_offsets.h` entries already get.

## Letting the compiler compute the offsets (2026-09-14)

Fixing the hand-packer raised the obvious question: why pack by hand at all?
Upstream's headers are the declarations. They do not *contain* offsets only
because in C++ an offset is implicit -- the compiler derives it. So derive it.

`tools/compile_layouts.py` extracts upstream's struct and enum declarations,
compiles them for arm64 against a shim carrying this build's verified container
widths, and reads the offsets out of clang's `-fdump-record-layouts`.

Upstream's tree will not compile here as-is: `Base.h` pulls in MSVC
`__try`/`__except`, Windows types and Noesis (`NsGui/`, `NsCore/`) headers we do
not have. Hence extraction rather than `#include`. A type we fail to supply is a
*compile error*, which is the whole point -- the old pipeline's failure mode was
a silently wrong offset, this one's is a diagnostic.

The shim needs no facts the hand-packer did not already need (`STDString` 16,
`Array` 16, `HashMap` 0x40). What it buys is everything the packer could not
model: base classes and vtables, bitfields, `std::array`/`optional`/`variant`,
padding, and **real enum widths** -- upstream declares every one of them
(`BEGIN_ENUM(DamageType, uint8_t)`), and guessing them at 4 bytes was what forced
251 components to truncate to nothing.

Skipping a field also stopped being dangerous. A field Lua cannot surface is
simply not emitted; clang has already placed everything after it.

### A third oracle

Upstream annotates renamed fields with the name they used to carry:

    [[bg3::legacy(field_4)]] FixedString OwnerProfileID;

That encodes an offset exactly as a bare `field_XX` does, but covers fields with
*real* names -- the ones mods actually read. 425 of them exist, and they are the
only offset check that touches named fields at all.

### Which oracle wins, and why it flipped

For the hand-packer, Windows self-naming was the stronger check. For compiled
output it is the weaker one, and deliberately so: the packer used Windows-derived
widths, so Windows offsets were the right yardstick, whereas the compiler uses
*our ARM64* widths, so the size this binary reports is. Measured over components
carrying both signals:

| | count |
| --- | --- |
| ARM64 size matches and every self-named offset matches | 89 |
| ARM64 size matches but a Windows self-name disagrees | 20 |
| ARM64 size differs but self-names all match | 1 |
| both disagree | 3 |

The 20 are MSVC/libc++ container widths genuinely differing, so the Windows
offset is the stale figure. A layout therefore ships on the ARM64 size, and falls
back to self-naming only when the binary reports no size for it.

### Result

335 compiled layouts, against the hand-packer's 213. Size agreement went from
287/333 (86%) to 458/471 (97%); 449 of 558 individual offset checks land exactly,
with the residue concentrated in the expected Windows/ARM64 divergence.

The strongest evidence is the cross-check: across the 29 components that also
have a hand-verified layout, **67 fields agree and none disagree** -- clang
independently reproduces every offset established by hand. Against the
hand-packer it agrees on 366 fields and differs on 2, and upstream's own
`[[bg3::legacy]]` annotations side with the compiler in both:

| | hand-packed | compiled | upstream says |
| --- | --- | --- | --- |
| `eoc::user::AvatarComponent.UserID` | 0x4 | 0x0 | 0x0 (`UserID` is the first member) |
| `eoc::PassiveComponent.PassiveId` | 0x18 | 0x4 | 0x4 (`legacy(field_0)` on `Type`) |

Lookup order is now hand-verified, then compiled, then hand-packed. 663 distinct
components carry a layout, up from 598, with zero fields past the end of their
component across all 2560 properties.

Not yet done: 1005 structs still fail to compile (mostly types the shim lacks),
140 layouts are uncorroborated because the binary reports no size for them, and
13 fail the size check outright.

### The extractor was silently truncating (2026-09-14)

The first cut of `strip_body` worked line by line, which is not sufficient for
C++. Three defects, each of which failed quietly:

1. **Method bodies outlived their signatures.** Dropping the line `T* alloc(...)`
   left `{ ... return ptr; }` behind as loose statements and orphaned braces.
2. **`#if 0` blocks were split.** The unit splitter could drop an `#endif` with
   the surrounding unit, leaving an unterminated conditional. Clang then consumed
   the ENTIRE remainder of the translation unit -- roughly 3000 lines of struct
   definitions and every `static_assert` forcing their layout -- and said so with
   a single `expected '}'` pointing at end of file. Record output fell from 2547
   to 682 with nothing indicating why.
3. **`DEFINE_COMPONENT(...)` has no trailing semicolon.** The splitter ran past
   it hunting for `;` and swallowed the member declared next, so every component
   lost its first field. 458 size-confirmed layouts collapsed to 15.

Two more, once the structure was right and members were being classified:

- Attributes carry parentheses. Testing for a parameter list before stripping
  `[[bg3::legacy(field_0)]]` deleted every annotated member -- exactly the 425
  named fields the legacy oracle exists to check.
- A function *pointer* is a data member. `void (*Callback)(int);` has parens but
  occupies 8 bytes, and dropping it shortened the struct.

All five present as a plausible-looking layout with a quietly wrong size, which
is the same failure class the audit was opened to find. The size oracle caught
every one -- a struct that loses a member stops reproducing the size the binary
reports -- which is the argument for gating on it rather than trusting output.

`strip_body` is now brace-aware: it splits a body into top-level units, keeps
data members and nested type definitions, and drops functions with their bodies
intact. Preprocessor directives and registration macros come off first, by
balanced parens rather than by line.

Net: 322 compiled layouts, 441 size-confirmed, 446 of 562 offset checks exact,
and still **66 fields agreeing with the hand-verified layouts and none
disagreeing**. 654 distinct components carry a layout; zero fields run past the
end across 2482 properties.

Remaining: 539 compile errors, mostly in non-component structs; 143 layouts the
binary reports no size for; 33 that fail the size check.
