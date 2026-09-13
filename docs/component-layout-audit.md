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

Fixed in this pass (element type taken from upstream's declaration; sizes are
fixed and certain, though no populated instance was available to verify element
decoding live):

| Component | Field | Upstream | Element |
|---|---|---|---|
| `eoc::VoiceTagComponent` | `Tags` | `Array<Guid>` | GUID/16 |
| `eoc::god::TagComponent` | `Tags` | `Array<Guid>` | GUID/16 |
| `eoc::combat::IsThreatenedComponent` | `ThreatenedBy` | `Array<EntityHandle>` | HANDLE/8 |
| `eoc::ObjectInteractionComponent` | `Interactions` | `Array<EntityHandle>` | HANDLE/8 |

Still opaque, with upstream's declared element type where known. Those needing a
struct layout also need the stride MEASURED live — upstream's legacy `field_NN`
names imply sizes this build does not use, and have misled this port before:

| Component | Field | Upstream element |
|---|---|---|
| `eoc::spell::CCPrepareSpellComponent` | `Spells` | `SpellMetaId` (layout exists: `g_SpellMetaId_Layout`, stride 0x30) |
| `eoc::spell::PlayerPrepareSpellComponent` | `Spells` | `SpellMetaId` (same) |
| `eoc::spell::AddedSpellsComponent` | `Spells` | `SpellMeta` |
| `eoc::spell::BookCooldownsComponent` | `Cooldowns` | ? |
| `eoc::TurnOrderComponent` | `TurnOrderIndices`, `TurnOrderIndices2` | `Array<uint64_t>` (no UINT64 element type yet) |
| `eoc::UseComponent` | `Requirements`, `Boosts`, `BoostsOnEquipMainHand`, `BoostsOnEquipOffHand` | ? |
| `eoc::UseBoostsComponent` | `Boosts` | `BoostDescription` |
| `eoc::ACOverrideFormulaBoostComponent` | `AddAbilityModifiers` | ? |
| `eoc::DifficultyCheckComponent` | `Abilities`, `field_30` | ? |
| `eoc::WeaponDamageResistanceBoostComponent` | `DamageTypes` | ? |
| `eoc::action::ActionUseConditionsComponent` | `Conditions` | ? |
| `eoc::character_creation::LevelUpComponent` | `LevelUps` | ? |
| `eoc::lock::LockComponent` | `field_18` | ? |
| `esv::StatesComponent` | `States` | `State` (shapeshift; large, optional-heavy) |
| `esv::ActivationGroupContainerComponent` | `Groups` | ? |
| `esv::BaseDataComponent` | `Resistances` | ? |
| `esv::BaseWeaponComponent` | `DamageList` | ? |
| `esv::ChasmDataComponent` | `Data` | ? |
| `esv::ConstellationHelperComponent` | `Data` | ? |
| `esv::CustomStatsComponent` | `Stats` | `LegacyMap<FixedString,int>` (not an array) |
| `esv::IconListComponent` | `Icons` | `IconInfo` |
| `esv::InterruptDataComponent` | `Data` | ? |
| `esv::InventoryOwnerComponent` | `Inventories` | ? |
| `esv::OsirisPingRequestSingletonComponent` | `Requests` | ? |
| `esv::PingRequestSingletonComponent` | `Requests` | ? |
| `esv::SummonContainerComponent` | `Summons` | ? |
| `esv::SurfacePathInfluencesComponent` | `PathInfluences` | ? |
| `esv::inventory::ShapeshiftAddedEquipmentComponent` | `Equipment` | ? |
| `esv::inventory::ShapeshiftEquipmentHistoryComponent` | `History` | ? |
| `esv::inventory::ShapeshiftUnequippedEquipmentComponent` | `Equipment` | ? |
| `ls::animation::DynamicAnimationTagsComponent` | `Tags` | `AnimationTag` |
| `ls::animation::LoadAnimationSetGameplayRequestOneFrameComponent` | `Animations` | ? |
| `ls::animation::RemoveAnimationSetsGameplayRequestOneFrameComponent` | `AnimationSets` | ? |
| `ls::animation::TemplateAnimationSetOverrideComponent` | `Overrides` | ? |
| `ls::trigger::IsInsideOfComponent` | `Triggers` | ? |
| `ls::uuid::ToHandleMappingComponent` | `Mappings` | ? |

## How to measure a struct element's stride

Expose the array's raw buffer and count read-only (`buf @0x00`, `size @0x0c`),
hex dump it, and find where the next element's header repeats. Confirm on two
different entities. This is how `esv::DisplayName`'s 0x50 stride was established
after the header's legacy names implied a different `TranslatedString` size than
this build uses.
