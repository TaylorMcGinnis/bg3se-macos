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

Only the two `SpellMetaId` arrays had a populated instance to check: both read 5
entries with `[1].OriginatorPrototype = "Target_Sanctuary"`. The others read
their array headers correctly but their element decoding is inferred from
upstream's declaration, not observed.

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
