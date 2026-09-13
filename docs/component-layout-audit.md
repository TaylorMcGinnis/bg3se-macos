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

Verified live by scanning every entity carrying the component
(`Ext.Entity.GetAllEntitiesWithComponent`, far better than checking party
members — the first two attempts found nothing because boosts live on items):

- `SpellMetaId` arrays: 5 entries, `[1].OriginatorPrototype = "Target_Sanctuary"`
- `UseBoosts.Boosts`: `[1].Boost = "Advantage"`, `.Params = "AttackRoll"`
- `IconList.Icons`: `[1].Icon = "Item_CONT_HAG_ThornyBush"`, `.field_4 = 4`

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
`BoostDescription` (4 uses), `IconInfo`, `AnimationTag`, `LevelUpData`,
`SurfacePathInfluence`, `BaseWeaponDamage`, `ActivationGroupData`,
`stats::Requirement`, `SpellMeta`, `AnimationWaterfallElement`, `State`.

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
