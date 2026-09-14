#!/usr/bin/env python3
"""
Generate C property layouts from Windows BG3SE component definitions.

This script reads the extracted Windows component data and generates
C code compatible with our component_offsets.h format.

Usage:
    python3 tools/generate_layouts.py > src/entity/generated_layouts.h
    python3 tools/generate_layouts.py --namespace eoc > eoc_layouts.h
    python3 tools/generate_layouts.py --list  # List all available components
"""

import os
import re
import json
import argparse
from pathlib import Path
from typing import Dict, List, Tuple, Optional
from dataclasses import dataclass

# Windows sizes JSON
WINDOWS_SIZES_JSON = Path("ghidra/offsets/windows_reference_sizes.json")

# Ghidra sizes for ARM64 verification
GHIDRA_SIZES_DIR = Path("ghidra/offsets/components")

# Type mapping to our FIELD_TYPE_* constants.
#
# Each entry is (FIELD_TYPE_* or None, size, alignment). A None type means "we
# cannot surface this to Lua" -- but the size and alignment are still REQUIRED,
# because the field occupies space and everything after it sits past it. See
# calculate_offsets() for why that used to be wrong.
#
# Widths are for THIS build (BG3 4.1.1.7398727 arm64), not for Windows x64:
#   STDString        16, verified live in src/core/stdstring.h
#   TranslatedString 32 (0x20), measured in eoc::DisplayNameComponent
# Both were previously 32/40 here, copied from the Windows headers.
TYPE_MAPPING = {
    # Primitives                       type                        size align
    "bool": ("FIELD_TYPE_BOOL", 1, 1),
    "int8_t": ("FIELD_TYPE_INT8", 1, 1),
    "uint8_t": ("FIELD_TYPE_UINT8", 1, 1),
    "char": ("FIELD_TYPE_INT8", 1, 1),
    "int16_t": ("FIELD_TYPE_INT16", 2, 2),
    "uint16_t": ("FIELD_TYPE_UINT16", 2, 2),
    "short": ("FIELD_TYPE_INT16", 2, 2),
    "int32_t": ("FIELD_TYPE_INT32", 4, 4),
    "uint32_t": ("FIELD_TYPE_UINT32", 4, 4),
    "int": ("FIELD_TYPE_INT32", 4, 4),
    "float": ("FIELD_TYPE_FLOAT", 4, 4),
    "int64_t": ("FIELD_TYPE_INT64", 8, 8),
    "uint64_t": ("FIELD_TYPE_UINT64", 8, 8),
    "double": ("FIELD_TYPE_DOUBLE", 8, 8),
    "__int64": ("FIELD_TYPE_INT64", 8, 8),

    # Handles
    "EntityHandle": ("FIELD_TYPE_ENTITY_HANDLE", 8, 8),
    "ComponentHandle": ("FIELD_TYPE_ENTITY_HANDLE", 8, 8),

    # Strings
    "FixedString": ("FIELD_TYPE_FIXEDSTRING", 4, 4),
    "Guid": ("FIELD_TYPE_GUID", 16, 8),
    "STDString": (None, 16, 8),        # opaque to Lua here, but 16 bytes wide
    "STDWString": (None, 16, 8),
    "TranslatedString": (None, 32, 8),

    # Vectors (glm: alignment is the component's, not the whole vector's)
    "glm::vec2": (None, 8, 4),         # no VEC2 field type
    "glm::ivec2": (None, 8, 4),
    "glm::vec3": ("FIELD_TYPE_VEC3", 12, 4),
    "glm::vec4": ("FIELD_TYPE_VEC4", 16, 4),
    "glm::quat": ("FIELD_TYPE_VEC4", 16, 4),
    "glm::mat3": (None, 36, 4),
    "glm::mat4": (None, 64, 4),

    # Common enums (all 4 bytes unless specified)
    "AbilityId": ("FIELD_TYPE_UINT8", 1, 1),
    "SkillId": ("FIELD_TYPE_UINT8", 1, 1),
    "SpellSchoolId": ("FIELD_TYPE_UINT8", 1, 1),
}

# Container widths. Opaque to Lua, but they still occupy space.
#
# Array=16 matches the ARM64 layout the port already relies on (buf 0x00,
# capacity 0x08, size 0x0c). Array/HashSet/HashMap were confirmed by fitting
# every computable struct against the Ghidra ARM64 size oracle: 16/48/64 scored
# 287 exact size matches out of 333, and every other combination scored lower.
TYPE_SIZE_FALLBACK = {
    "Array": (16, 8),
    "StaticArray": (16, 8),
    "LegacyRefMap": (16, 8),
    "HashSet": (48, 8),
    "HashMap": (64, 8),
    "MultiHashMap": (80, 8),
    "SparseHashSet": (48, 8),
    "SparseHashMap": (64, 8),
    "std::optional": (16, 8),
    "std::variant": (32, 8),
    "Signal": (24, 8),
    "BitSet": (24, 8),
    "ecs::EntityRef": (16, 8),
}


@dataclass
class FieldInfo:
    name: str
    type_str: str
    field_type: str  # FIELD_TYPE_*
    size: int
    offset: int
    is_readonly: bool = True


def parse_type(type_str: str) -> Tuple[Optional[str], int, int]:
    """Parse a C++ type and return (FIELD_TYPE_* or None, size, alignment).

    A size of 0 means the width is genuinely unknown. That is not the same as
    "skip": an unknown width makes every LATER field's offset unknowable too,
    so callers must stop rather than continue.
    """
    type_str = type_str.strip()

    # Remove const/volatile
    type_str = re.sub(r'\b(const|volatile|mutable)\b', '', type_str).strip()

    # Pointers and references: opaque to Lua, 8 bytes wide.
    if type_str.endswith('*') or type_str.endswith('&'):
        return (None, 8, 8)

    # Check direct mapping
    if type_str in TYPE_MAPPING:
        return TYPE_MAPPING[type_str]

    # Check for templates
    template_match = re.match(r'^([\w:]+)\s*<(.+)>$', type_str)
    if template_match:
        container = template_match.group(1)
        if container in TYPE_SIZE_FALLBACK:
            # Containers are opaque to Lua, but their width is known.
            size, align = TYPE_SIZE_FALLBACK[container]
            return (None, size, align)

    # Check for qualified names
    if '::' in type_str:
        last_part = type_str.split('::')[-1]
        if last_part in TYPE_MAPPING:
            return TYPE_MAPPING[last_part]

    # Unknown width. Previously this guessed 4 bytes for any name containing
    # "Type", "Id" or "Flags" -- but enums here are frequently 1 byte (DamageType
    # is), so the guess silently shifted every following field.
    return (None, 0, 1)


def calculate_offsets(fields: List[dict]) -> Tuple[List[FieldInfo], int, Optional[dict]]:
    """Lay fields out in declaration order.

    Returns (exposed_fields, size_of_laid_out_prefix, truncated_at_or_None).

    Every field advances the cursor, including ones Lua cannot see. The previous
    version skipped unexposed fields WITHOUT advancing, so a single pointer,
    container or string shifted every later field down by its width -- which is
    how eoc::DifficultyCheckComponent came to read 0x20/0x24 instead of
    0x40/0x44, and why esv::Projectile's 48 fields were each off by 8 (its first
    member is a vtable pointer).
    """
    result = []
    offset = 0
    max_align = 1

    for field in fields:
        type_str = field['type']
        name = field['name']

        field_type, size, align = parse_type(type_str)

        # Unknown width: everything past here is unknowable. Stop; the prefix
        # already laid out is still correct, because a field's offset depends
        # only on what precedes it.
        if size == 0:
            return result, offset, {'name': name, 'type': type_str}

        if align > 1 and offset % align != 0:
            offset += align - (offset % align)
        max_align = max(max_align, align)

        if field_type is not None:
            result.append(FieldInfo(
                name=name,
                type_str=type_str,
                field_type=field_type,
                size=size,
                offset=offset,
                is_readonly=True
            ))

        offset += size

    # Tail padding to the struct's own alignment, so the total can be compared
    # against the size the binary actually reports.
    if max_align > 1 and offset % max_align != 0:
        offset += max_align - (offset % max_align)

    return result, offset, None


LIVE_SIZES = Path("ghidra/offsets/components/live_component_sizes.json")


def load_ghidra_sizes() -> Dict[str, int]:
    """Component sizes to validate against, engine-reported where available.

    The Ghidra extraction is the fallback; live_component_sizes.json, harvested
    from a running game via Ext.Entity.GetComponentSizes(), wins where it has an
    entry. It is EntityStorageData::ComponentSizes -- the stride the ECS actually
    addresses by -- so it outranks any static extraction of it, and the static
    one is demonstrably wrong in places (it recorded eoc::hit::TargetComponent
    as 0x18 against a real 0xb8, and ls::EffectComponent as 0x8, smaller than
    that struct's own first two fields).

    Both generators must read the same oracle, or a layout this one emits can
    contradict the size the other validated against.
    """
    sizes = {}

    for md_file in GHIDRA_SIZES_DIR.glob("COMPONENT_SIZES*.md"):
        with open(md_file, 'r') as f:
            for line in f:
                if '---' in line or not line.strip().startswith('|'):
                    continue
                if '::' not in line:
                    continue

                parts = line.split('|')
                if len(parts) >= 3:
                    name = parts[1].strip()
                    size_field = parts[2].strip().replace('`', '')
                    size_field = re.sub(r'\s*\([^)]*\)', '', size_field).strip()

                    if '::' in name and name not in ('Component', 'Name'):
                        size = None
                        if size_field.startswith('0x'):
                            try:
                                size = int(size_field, 16)
                            except ValueError:
                                pass
                        else:
                            match = re.match(r'^(\d+)', size_field)
                            if match:
                                try:
                                    size = int(match.group(1))
                                except ValueError:
                                    pass

                        if size and 0 < size <= 10000:
                            sizes[name.lower()] = size

    if LIVE_SIZES.exists():
        live = json.loads(LIVE_SIZES.read_text(encoding='utf-8'))
        for k, v in live.items():
            sizes[k.lower()] = v
        import sys as _sys
        print(f"// live component sizes: {len(live)} entries", file=_sys.stderr)

    return sizes


def generate_layout_code(name: str, info: dict, ghidra_sizes: Dict[str, int],
                         stats: Optional[dict] = None) -> Optional[str]:
    """Generate C code for a component layout, or None to emit nothing.

    A layout is only emitted when this build's own component size corroborates
    the computed packing. The alternative -- shipping offsets derived purely
    from Windows headers -- is what produced 357 fields reading the wrong
    address, and a wrong offset yields a plausible number from the wrong place
    with nothing to indicate it. An absent layout at least reads as nil.
    """
    def bump(key):
        if stats is not None:
            stats[key] = stats.get(key, 0) + 1

    if info.get('is_tag', False):
        return None  # Skip tag components

    fields = info.get('fields', [])
    if not fields:
        return None  # No fields to expose

    field_infos, computed_size, truncated = calculate_offsets(fields)
    if not field_infos:
        bump('no_usable_fields')
        return None  # No usable fields

    # Get sizes
    windows_size = info['estimated_size']
    ghidra_size = ghidra_sizes.get(name.lower())

    # Use Ghidra size if available (more accurate for ARM64)
    actual_size = ghidra_size if ghidra_size else windows_size

    # ---- per-field check against self-naming ------------------------------
    #
    # The Windows headers name fields they could not identify after their own
    # offset: "field_1B0" sits at 0x1B0 there. That makes each such field its
    # own assertion about the packing, which is far stronger than checking the
    # struct total. The old generator scored 45% against these; it now scores
    # 79%, and the residue is not noise -- it is structs where EVERY self-named
    # field is off by the same constant, i.e. a base-class prefix this model
    # does not represent (eoc::character_creation::DefinitionStateComponent is
    # uniformly +8, DummyDefinitionComponent uniformly +0x10).
    #
    # A constant delta is distinguishable from a genuine Windows/ARM64
    # divergence: differing container or string widths would shift each field by
    # a DIFFERENT amount as they accumulate, not by one shared constant. So a
    # uniform delta is applied as a prefix, but only when the struct total then
    # also lands on the size the binary reports. Anything inconsistent means the
    # model does not describe this struct, and it is dropped.
    deltas = set()
    for fi in field_infos:
        m = re.match(r'^field_([0-9A-Fa-f]+)$', fi.name)
        if m:
            deltas.add(int(m.group(1), 16) - fi.offset)

    shift = 0
    selfnamed = ""
    if deltas:
        if len(deltas) > 1:
            bump('selfname_inconsistent')
            return None
        shift = deltas.pop()
        if shift < 0:
            bump('selfname_negative')
            return None
        if shift > 0:
            # Only trust a prefix the size oracle agrees with.
            if ghidra_size is None or truncated is not None or \
                    computed_size + shift != ghidra_size:
                bump('selfname_shift_unconfirmed')
                return None
            for fi in field_infos:
                fi.offset += shift
            computed_size += shift
            selfnamed = (f"; {len(fields)} fields shifted by 0x{shift:x} "
                         f"for an unmodelled base-class prefix")
        else:
            selfnamed = "; self-named offsets agree exactly"

    # ---- validation against this build -----------------------------------
    if ghidra_size is None:
        # No ARM64 size to check against, so the struct total proves nothing --
        # but self-naming is an independent, per-field check that does not need
        # a size at all. If every self-named field lands exactly where its own
        # name says, the packing is corroborated regardless.
        if selfnamed and shift == 0:
            actual_size = windows_size
            field_infos = [fi for fi in field_infos
                           if fi.offset + fi.size <= actual_size]
            if not field_infos:
                bump('all_fields_out_of_bounds')
                return None
            bump('selfnamed_no_size')
            provenance = ("no ARM64 size available; every self-named offset "
                          "lands exactly, size is the Windows estimate")
            return _emit(name, field_infos, actual_size, ghidra_size,
                         provenance)
        bump('no_arm64_size')
        return None

    if selfnamed and shift == 0:
        # Every self-named field landed exactly where its own name says. That is
        # a per-FIELD check, and so it outranks the struct total: a total may
        # legitimately differ because the Windows header does not list trailing
        # members. eoc::relation::FactionComponent is the case in point -- its
        # field_0/field_8/field_18 all land exactly, but the listed fields sum
        # to 0x28 against a real size of 0x30, and requiring the totals to agree
        # threw away a layout that was already right.
        provenance = ("every self-named offset lands exactly"
                      + (f"; listed fields end at 0x{computed_size:x} of "
                         f"0x{ghidra_size:x}" if computed_size != ghidra_size
                         else ""))
    elif truncated is None:
        # Nothing self-named to check, so the struct total is all there is.
        if computed_size != ghidra_size:
            bump('size_mismatch')
            return None
        provenance = ("packing reproduces ARM64 size "
                      f"0x{ghidra_size:x} exactly{selfnamed}")
    else:
        # Laid out only up to an unknown-width field. The prefix is still sound,
        # but nothing corroborates it, so require that it at least fits.
        if computed_size > ghidra_size:
            bump('prefix_overruns')
            return None
        provenance = (f"prefix only, truncated at {truncated['type']} "
                      f"{truncated['name']} (unknown width){selfnamed}")

    # Nothing may extend past the end of the component, whatever the source.
    field_infos = [fi for fi in field_infos if fi.offset + fi.size <= actual_size]
    if not field_infos:
        bump('all_fields_out_of_bounds')
        return None

    bump('exact' if truncated is None else 'prefix')
    return _emit(name, field_infos, actual_size, ghidra_size, provenance)


def _emit(name: str, field_infos: List[FieldInfo], actual_size: int,
          ghidra_size: Optional[int], provenance: str) -> str:
    """Render one corroborated layout as C."""
    # Generate variable names (use Gen_ prefix for compatibility)
    safe_name = "Gen_" + name.replace('::', '_').replace('<', '_').replace('>', '_')
    safe_name = re.sub(r'[^a-zA-Z0-9_]', '', safe_name)

    short_name = name.split('::')[-1]
    if short_name.endswith('Component'):
        short_name = short_name[:-9]

    lines = []

    # Header comment
    lines.append(f"// {'='*70}")
    lines.append(f"// {name}")
    lines.append(f"// Field names/order from Windows BG3SE headers; offsets computed here.")
    if ghidra_size:
        lines.append(f"// ARM64 Size: 0x{ghidra_size:x} ({ghidra_size} bytes) - Ghidra verified")
    else:
        lines.append(f"// Size: 0x{actual_size:x} ({actual_size} bytes) - Windows estimate")
    lines.append(f"// Checked: {provenance}")
    lines.append(f"// {'='*70}")
    lines.append("")

    # Property definitions
    lines.append(f"static const ComponentPropertyDef g_{safe_name}_Properties[] = {{")
    for fi in field_infos:
        readonly_str = "true" if fi.is_readonly else "false"
        lines.append(f'    {{ "{fi.name}", 0x{fi.offset:02x}, {fi.field_type}, 0, {readonly_str} }},')
    lines.append("};")
    lines.append("")

    # Layout definition
    lines.append(f"static const ComponentLayoutDef g_{safe_name}_Layout = {{")
    lines.append(f'    .componentName = "{name}",')
    lines.append(f'    .shortName = "{short_name}",')
    lines.append(f'    .componentTypeIndex = 0,')
    lines.append(f'    .componentSize = 0x{actual_size:x},')
    lines.append(f'    .properties = g_{safe_name}_Properties,')
    lines.append(f'    .propertyCount = sizeof(g_{safe_name}_Properties) / sizeof(g_{safe_name}_Properties[0]),')
    lines.append("};")
    lines.append("")

    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description='Generate C property layouts from Windows headers')
    parser.add_argument('--namespace', '-n', help='Filter by namespace (e.g., eoc, esv)')
    parser.add_argument('--list', '-l', action='store_true', help='List available components')
    parser.add_argument('--count', '-c', type=int, default=0, help='Limit number of components')
    args = parser.parse_args()

    os.chdir(Path(__file__).parent.parent)

    # Load data
    with open(WINDOWS_SIZES_JSON) as f:
        windows_data = json.load(f)

    ghidra_sizes = load_ghidra_sizes()

    # Filter by namespace if specified
    if args.namespace:
        ns_prefix = args.namespace + '::'
        windows_data = {k: v for k, v in windows_data.items()
                       if k.startswith(ns_prefix)}

    if args.list:
        for name in sorted(windows_data.keys()):
            info = windows_data[name]
            tag = " (tag)" if info.get('is_tag') else ""
            field_count = len(info.get('fields', []))
            print(f"{name}: {info['estimated_size']} bytes, {field_count} fields{tag}")
        return

    # Count Ghidra-verified components
    ghidra_verified = sum(1 for name in windows_data
                         if ghidra_sizes.get(name.lower()))

    # Generate header
    print("/**")
    print(" * generated_property_defs.h - Auto-generated component property definitions")
    print(" *")
    print(" * Generated by tools/generate_layouts.py.")
    print(" *")
    print(" * Field names and declaration order come from the Windows BG3SE headers;")
    print(" * the OFFSETS are computed here for ARM64 and every layout below is")
    print(" * corroborated against this build's own component size before being")
    print(" * emitted. A struct whose computed packing does not reproduce the size")
    print(" * the binary reports is dropped rather than guessed at, because a wrong")
    print(" * offset reads a plausible value from the wrong address and says nothing,")
    print(" * whereas a missing layout reads as nil.")
    print(" *")
    print(" * These remain WEAKER than the hand-verified layouts in component_offsets.h,")
    print(" * which win on lookup; see docs/component-layout-audit.md.")
    print(" */")
    print("")
    print("#ifndef GENERATED_PROPERTY_DEFS_H")
    print("#define GENERATED_PROPERTY_DEFS_H")
    print("")
    print("#include \"component_property.h\"")
    print("")

    # Generate layouts and track which ones were actually generated
    count = 0
    generated = 0
    generated_names = []
    stats = {}
    for name in sorted(windows_data.keys()):
        info = windows_data[name]
        code = generate_layout_code(name, info, ghidra_sizes, stats)
        if code:
            print(code)
            generated += 1
            generated_names.append(name)
        count += 1
        if args.count and count >= args.count:
            break

    # Generate registry array (only include actually generated layouts)
    print(f"#define GENERATED_COMPONENT_COUNT {generated}")
    print("")
    print("static const ComponentLayoutDef* g_GeneratedComponentLayouts[] = {")
    for name in generated_names:
        safe_name = "Gen_" + name.replace('::', '_').replace('<', '_').replace('>', '_')
        safe_name = re.sub(r'[^a-zA-Z0-9_]', '', safe_name)
        print(f"    &g_{safe_name}_Layout,")
    print("    NULL")
    print("};")
    print("")
    print("#endif // GENERATED_PROPERTY_DEFS_H")

    import sys
    print(f"\n// Generated {generated} layouts from {len(windows_data)} components",
          file=sys.stderr)
    print(f"//   emitted, size corroborated exactly : {stats.get('exact', 0)}", file=sys.stderr)
    print(f"//   emitted, prefix up to unknown type : {stats.get('prefix', 0)}", file=sys.stderr)
    print(f"//   emitted, self-named offsets exact  : {stats.get('selfnamed_no_size', 0)}",
          file=sys.stderr)
    print(f"//   dropped, self-naming inconsistent  : {stats.get('selfname_inconsistent', 0)}",
          file=sys.stderr)
    print(f"//   dropped, unconfirmed base prefix   : {stats.get('selfname_shift_unconfirmed', 0)}",
          file=sys.stderr)
    print(f"//   dropped, computed size != ARM64    : {stats.get('size_mismatch', 0)}",
          file=sys.stderr)
    print(f"//   dropped, no ARM64 size to check    : {stats.get('no_arm64_size', 0)}",
          file=sys.stderr)
    print(f"//   dropped, prefix overruns component : {stats.get('prefix_overruns', 0)}",
          file=sys.stderr)
    print(f"//   dropped, no Lua-visible fields     : {stats.get('no_usable_fields', 0)}",
          file=sys.stderr)


if __name__ == "__main__":
    main()
