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

## Lua-name mismatches: 99, and they are COSMETIC

99 hand-written layouts carry a `shortName` that differs from upstream's
`DEFINE_COMPONENT` name (e.g. ours `AttributeFlagsComponent`, upstream
`AttributeFlags`). **These are not bugs.** Entity lookup falls back through the
generated upstream name table, so both names resolve — verified live on
`CharacterCreationStats`, `AttributeFlags`, `BodyType`, `Voice`,
`ApprovalRatings`, `CharacterCreationAppearance`.

Do not mass-rename them. The only reason to touch one is if a *specific* lookup
is shown to fail.

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
`SurfacePathInfluence` (leading enum of unknown width),
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

## How to measure a struct element's stride

Expose the array's raw buffer and count read-only (`buf @0x00`, `size @0x0c`),
hex dump it, and find where the next element's header repeats. Confirm on two
different entities. This is how `esv::DisplayName`'s 0x50 stride was established
after the header's legacy names implied a different `TranslatedString` size than
this build uses.
