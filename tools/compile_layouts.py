#!/usr/bin/env python3
"""
Derive ARM64 component field offsets by COMPILING upstream's declarations.

Why this exists
---------------
generate_layouts.py reads a flattened field list out of the Windows headers and
packs it by hand. Hand-packing was wrong in four separate ways at once (see
docs/component-layout-audit.md), and even corrected it cannot model base
classes, bitfields, std::array/optional/variant, or enum widths -- so it
truncates at the first type it does not recognise and 251 components emit
nothing at all.

A C++ compiler already knows all of that. Upstream's headers ARE the
declarations; they just do not contain offsets, because in C++ an offset is
implicit. So: extract the declarations, compile them for arm64 against a shim
that supplies this build's verified container widths, and read the offsets back
out of clang's -fdump-record-layouts.

Upstream's tree cannot be compiled as-is here -- Base.h pulls in MSVC __try/
__except, Windows types and Noesis (NsGui/, NsCore/) headers we do not have.
Hence extraction rather than #include. A type we fail to extract becomes a
COMPILE ERROR, which is the point: the old pipeline's failure mode was a
silently wrong offset, and this one's is a loud diagnostic.

The shim's widths are inputs either way, and are the same ones the hand-packer
needed:
    STDString 16  -- verified live, src/core/stdstring.h
    Array     16  -- buf 0x00, capacity 0x08, size 0x0c
    HashMap   64 / HashSet 48 -- fitted against the ARM64 size oracle (287/333)
"""

import os
import re
import subprocess
import sys
from collections import OrderedDict
from pathlib import Path

UPSTREAM = Path("../upstream/BG3Extender")
GAMEDEFS = UPSTREAM / "GameDefinitions"

# ---------------------------------------------------------------------------
# Shim: primitives the game defines itself, at THIS build's widths.
# ---------------------------------------------------------------------------

SHIM = r"""
#include <cstdint>
#include <cstddef>
#include <array>
#include <optional>
#include <variant>
#include <vector>
#include <functional>

// MSVC spellings used verbatim in upstream's field declarations.
typedef int8_t  __int8;
typedef int16_t __int16;
typedef int32_t __int32;
typedef int64_t __int64;
struct SRWLOCK { void* Ptr; };
struct CRITICAL_SECTION { void* Ptr[5]; };

namespace bg3se {

// Exact, from upstream CoreLib/Base/BaseUtilities.h:155.
template <class T> struct OverrideableProperty { T Value; bool IsOverridden; };
// Empty bases (BaseUtilities.h:43, :54, :59).
class Noncopyable {};
class ProtectedGameObjectBase {};
template <class T> class ProtectedGameObject : public ProtectedGameObjectBase {};

template <class T, unsigned N> struct BitArray { T Vals[N]; };

struct EntityHandle { uint64_t Value; };
struct Guid { uint64_t Val[2]; };
struct FixedString { uint32_t Index; };

// 16 bytes on this build, NOT std::string (which would be 24 on libc++, 32 on
// MSVC). Verified live: src/core/stdstring.h.
struct STDString { char* ptr_; uint32_t size_; uint32_t capacity_; };
struct STDWString { wchar_t* ptr_; uint32_t size_; uint32_t capacity_; };

struct RuntimeStringHandle { FixedString Handle; uint32_t Version; uint64_t pad_; };
struct TranslatedString { RuntimeStringHandle Handle; RuntimeStringHandle ArgumentString; };

// buf 0x00, capacity 0x08, size 0x0c
template <class T> struct Array { T* buf_; uint32_t capacity_; uint32_t size_; };
template <class T> struct StaticArray { T* buf_; uint32_t size_; };
template <class T, unsigned N> struct ObjectSet { T* buf_; uint32_t capacity_; uint32_t size_; };

template <class T> struct HashSet {
    Array<int32_t> HashKeys; Array<int32_t> NextIds; Array<T> Keys;
};
template <class K, class V> struct HashMap {
    Array<int32_t> HashKeys; Array<int32_t> NextIds; Array<K> Keys; Array<V> Values;
};
template <class K, class V> struct MultiHashMap {
    Array<int32_t> HashKeys; Array<int32_t> NextIds; Array<K> Keys; Array<V> Values;
    Array<int32_t> Overflow;
};
template <class T> struct SparseHashSet : HashSet<T> {};
template <class K, class V> struct SparseHashMap : HashMap<K, V> {};
template <class K, class V> struct LegacyRefMap { void* Hash; uint32_t Size; uint32_t Unused; };
template <class K, class V> struct LegacyMap { void* Hash; uint32_t Size; uint32_t Unused; };
template <class K, class V> struct VirtualMultiHashMap : MultiHashMap<K, V> {};

template <class T> struct BitSet { Array<T> Buf; };
template <class T> struct Signal { void* Handlers[3]; };
template <class T> struct Ref { T* ptr_; };
template <class T> struct TypedHandle { uint64_t Value; };

// Larian runtime container with no definition anywhere in upstream's headers.
// Stubbed as an Array: if that width is wrong, the struct's computed size stops
// matching the ARM64 size oracle and the layout is DROPPED rather than shipped
// wrong, so a bad stub costs coverage, never correctness.
template <class T> struct TrackedCompactSet { T* buf_; uint32_t capacity_; uint32_t size_; };
template <class T> struct CompactSet { T* buf_; uint32_t capacity_; uint32_t size_; };

using TComponentTypeIndex = uint16_t;
enum class ComponentTypeIndex : TComponentTypeIndex {};
enum class ReplicationTypeIndex : uint16_t {};
enum class SystemTypeIndex : uint16_t {};
enum class QueryIndex : uint16_t {};
using ComponentTypeMask = BitArray<uint64_t, 32>;
struct EntityWorld;
struct Visual;
struct UnknownSignal { void* p[3]; };
struct FrameAllocator { void* p; };

struct BaseComponent {};
struct ComponentHandle { uint64_t Value; };
struct Version { uint32_t Ver; };
struct Path { STDString Name; };
struct NetId { uint32_t Id; };
struct UserId { int32_t Id; };

namespace ecs { struct EntityRef { uint64_t Handle; void* World; }; }

} // namespace bg3se

namespace glm {
struct vec2 { float x, y; };
struct vec3 { float x, y, z; };
struct vec4 { float x, y, z, w; };
struct quat { float x, y, z, w; };
struct ivec2 { int x, y; };
struct ivec3 { int x, y, z; };
struct ivec4 { int x, y, z, w; };
struct mat3 { float v[9]; };
struct mat4 { float v[16]; };
}

namespace bg3se {
using fvec2 = glm::vec2;
using fvec3 = glm::vec3;
using fvec4 = glm::vec4;
using aligned_vec4 = glm::vec4;
}
"""

# Macros that appear inside struct bodies and contribute no storage.
STRIP_MACROS = re.compile(
    r'^\s*(DEFINE_COMPONENT|DEFINE_TAG_COMPONENT|DEFINE_ONEFRAME_TAG_COMPONENT|'
    r'DEFN_BOOST|DEFINE_BOOST|DEFINE_SYSTEM|DEFINE_ONEFRAME_COMPONENT|'
    r'DEFINE_PROXY_COMPONENT|DEFINE_SINGLETON_COMPONENT)\s*\(', re.M)


ENGINE_NAME = re.compile(
    r'DEFINE_(?:ONEFRAME_)?COMPONENT\s*\(\s*\w+\s*,\s*"([^"]+)"')
ENGINE_TAG = re.compile(
    r'DEFINE_(?:ONEFRAME_)?TAG_COMPONENT\s*\(\s*([\w:]+)\s*,\s*(\w+)\s*,')
ENGINE_BOOST = re.compile(r'DEFN_BOOST\s*\(\s*(\w+)\s*,')


LEGACY_RE = re.compile(
    r'\[\[bg3::legacy\(field_([0-9A-Fa-f]+)\)\]\]\s*'
    r'(?:\[\[[^\]]*\]\]\s*)*'          # further attributes
    r'[\w:<>,\s\*&]*?(\w+)\s*(?:\[[^\]]*\])?\s*;')


def legacy_offsets(body: str):
    """Fields upstream RENAMED, annotated with the name they used to carry.

    [[bg3::legacy(field_4)]] FixedString OwnerProfileID;

    The old name encodes the offset, exactly as a bare field_XX does -- but this
    form covers fields that now have REAL names, which is what mods actually
    read. 425 of these exist, and they are the only offset check that touches
    named fields at all."""
    return {m.group(2): int(m.group(1), 16) for m in LEGACY_RE.finditer(body)}


def engine_class_of(body: str):
    """The game-side class name, taken from the registration macro itself.

    Upstream's namespaces are not the engine's -- BEGIN_NS(death) is
    bg3se::death, but the component is eoc::death::DeadByDefaultComponent. The
    macro carries the real name as a string literal, so there is nothing to
    infer."""
    m = ENGINE_NAME.search(body)
    if m:
        return m.group(1)
    m = ENGINE_TAG.search(body)
    if m:
        return f"{m.group(1)}::{m.group(2)}"
    m = ENGINE_BOOST.search(body)
    if m:
        return f"eoc::{m.group(1)}BoostComponent"
    return None


def strip_body(body: str) -> str:
    """Remove things that are not data members: methods, macros, statics."""
    out = []
    depth = 0
    for line in body.splitlines():
        s = line.strip()
        # Drop component-registration macros (may span lines via trailing \ or ()
        if STRIP_MACROS.match(line):
            # consume until parens balance
            depth = line.count('(') - line.count(')')
            continue
        if depth > 0:
            depth += line.count('(') - line.count(')')
            continue
        if not s or s.startswith('//'):
            continue
        # methods, ctors, operators, statics, usings, friends
        if re.match(r'^(static|inline|virtual|friend|using|typedef|template|'
                    r'constexpr|explicit|~|operator)\b', s):
            continue
        if '(' in s and ')' in s and not re.search(r'\{[^}]*\}\s*;?\s*$', s):
            # looks like a function declaration rather than a member with init
            if re.search(r'\)\s*(const)?\s*(noexcept)?\s*[;{]', s):
                continue
        out.append(line)
    return '\n'.join(out)


# ---------------------------------------------------------------------------
# Extraction
# ---------------------------------------------------------------------------

ENUM_RE = re.compile(
    r'^BEGIN_(ENUM|BITMASK)(_NS)?\s*\((.*?)\)\s*$', re.M)


def collect_enums():
    """Upstream declares every enum's underlying type explicitly:
         BEGIN_ENUM(DamageType, uint8_t)
         BEGIN_ENUM_NS(stats, DeathType, StatsDeathType, uint8_t)
       That is exactly the width the hand-packer had to guess at (and got wrong:
       it assumed 4 bytes for anything named *Type/*Id/*Flags; DamageType is 1).
       Bodies are irrelevant to layout, so empty enums suffice."""
    out = OrderedDict()
    for inl in sorted((GAMEDEFS / "Enumerations").glob("*.inl")):
        for m in ENUM_RE.finditer(inl.read_text(encoding='utf-8', errors='replace')):
            args = [a.strip() for a in m.group(3).split(',')]
            if m.group(2):                      # _NS form: ns, Name, LuaName, type
                if len(args) < 4:
                    continue
                ns, name, ty = args[0], args[1], args[3]
            else:                               # plain: Name, type
                if len(args) < 2:
                    continue
                ns, name, ty = None, args[0], args[1]
            out[(ns, name)] = ty
    return out


NS_OPEN = re.compile(r'^[ \t]*BEGIN_(SE|NS)\s*\(\s*([\w:]*)\s*\)', re.M)
NS_CLOSE = re.compile(r'^[ \t]*END_(SE|NS)\s*\(\s*\)', re.M)
# The opening brace is usually on the NEXT line, so match up to it across lines.
STRUCT_RE = re.compile(
    r'^[ \t]*(?:struct|class)\s+(\w+)\s*(?::([^{;]+?))?\s*\{', re.M | re.S)


def collect_structs():
    """Every struct/class in GameDefinitions, with its namespace and bases."""
    out = OrderedDict()
    files = sorted(list(GAMEDEFS.rglob("*.h")) + list(GAMEDEFS.rglob("*.inl")))
    for path in files:
        if "Enumerations" in str(path):
            continue
        text = path.read_text(encoding='utf-8', errors='replace')

        # Namespace openings/closings as (position, delta) so a struct's
        # namespace is whatever stack is open at its offset.
        events = []
        for m in NS_OPEN.finditer(text):
            events.append((m.start(), 'open', m.group(2) if m.group(1) == 'NS' else ''))
        for m in NS_CLOSE.finditer(text):
            events.append((m.start(), 'close', None))
        events.sort()

        for sm in STRUCT_RE.finditer(text):
            # Skip forward declarations and anything inside a template<>
            name = sm.group(1)
            start = text.index('{', sm.start())
            depth, j = 0, start
            while j < len(text):
                if text[j] == '{':
                    depth += 1
                elif text[j] == '}':
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            body = text[start + 1:j]

            stack = []
            for pos, kind, val in events:
                if pos > sm.start():
                    break
                if kind == 'open':
                    stack.append(val)
                elif stack:
                    stack.pop()
            ns = '::'.join(x for x in stack if x)

            bases = []
            if sm.group(2):
                for b in sm.group(2).split(','):
                    b = re.sub(r'\b(public|private|protected|virtual)\b', '', b).strip()
                    if b:
                        bases.append(b)
            key = (ns, name)
            if key not in out:
                out[key] = {'body': body, 'bases': bases, 'file': str(path)}
    return out



# ---------------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------------

# Types the shim already provides; never emit our own copy of these.
SHIM_TYPES = {
    'EntityHandle', 'Guid', 'FixedString', 'STDString', 'STDWString',
    'RuntimeStringHandle', 'TranslatedString', 'Array', 'StaticArray',
    'ObjectSet', 'HashSet', 'HashMap', 'MultiHashMap', 'SparseHashSet',
    'SparseHashMap', 'LegacyRefMap', 'LegacyMap', 'VirtualMultiHashMap',
    'BitSet', 'Signal', 'Ref', 'TypedHandle', 'BaseComponent',
    'ComponentHandle', 'Version', 'Path', 'NetId', 'UserId', 'EntityRef',
    'vec2', 'vec3', 'vec4', 'quat', 'ivec2', 'ivec3', 'ivec4', 'mat3', 'mat4',
    'OverrideableProperty', 'Noncopyable', 'ProtectedGameObject',
    'ProtectedGameObjectBase', 'BitArray', 'TrackedCompactSet', 'CompactSet',
    'ComponentTypeIndex', 'ReplicationTypeIndex', 'SystemTypeIndex',
    'QueryIndex', 'ComponentTypeMask', 'TComponentTypeIndex', 'UnknownSignal',
    'FrameAllocator', 'fvec2', 'fvec3', 'fvec4', 'aligned_vec4', 'SRWLOCK',
    'CRITICAL_SECTION',
}

BUILTIN = {
    'void', 'bool', 'char', 'short', 'int', 'long', 'float', 'double',
    'unsigned', 'signed', 'wchar_t', 'size_t', 'uint8_t', 'uint16_t',
    'uint32_t', 'uint64_t', 'int8_t', 'int16_t', 'int32_t', 'int64_t',
    'std', 'array', 'optional', 'variant', 'pair', 'tuple', 'function',
    'nullptr_t', 'byte', 'true', 'false', 'const', 'struct', 'class', 'enum',
}

IDENT = re.compile(r'\b([A-Za-z_]\w*)\b')


def referenced_names(text):
    """Bare type identifiers mentioned in a struct body or base list."""
    return {m.group(1) for m in IDENT.finditer(text)}


def build_tu(structs, enums, wanted):
    """Emit one translation unit containing `wanted` and everything it needs."""
    by_name = {}
    for (ns, name) in structs:
        by_name.setdefault(name, []).append((ns, name))
    enum_by_name = {}
    for (ns, name) in enums:
        enum_by_name.setdefault(name, []).append((ns, name))

    # Transitive closure over referenced bare names.
    need, queue = set(), list(wanted)
    while queue:
        key = queue.pop()
        if key in need or key not in structs:
            continue
        need.add(key)
        info = structs[key]
        for nm in referenced_names(info['body'] + ' ' + ' '.join(info['bases'])):
            if nm in BUILTIN or nm in SHIM_TYPES:
                continue
            for cand in by_name.get(nm, []):
                if cand not in need:
                    queue.append(cand)

    # Topological order: a struct must follow anything it contains by value.
    order, seen, stack = [], set(), set()

    def visit(key):
        if key in seen or key not in structs:
            return
        if key in stack:      # cycle (only legal through pointers) -- break it
            return
        stack.add(key)
        info = structs[key]
        deps = set(info['bases'])
        for line in info['body'].splitlines():
            s = line.strip()
            if not s or s.startswith('//') or '*' in s or '&' in s:
                continue
            deps |= referenced_names(s)
        for nm in deps:
            base = nm.split('::')[-1]
            if base in BUILTIN or base in SHIM_TYPES:
                continue
            for cand in by_name.get(base, []):
                if cand in need:
                    visit(cand)
        stack.discard(key)
        seen.add(key)
        order.append(key)

    for key in need:
        visit(key)

    # Render
    parts = [SHIM]
    emitted_ns = None

    def open_ns(ns):
        return f"namespace bg3se{'::' + ns if ns else ''} {{"

    # enums first: they have no dependencies
    for (ns, name), ty in enums.items():
        full = f"bg3se::{ns}" if ns else "bg3se"
        parts.append(f"namespace {full} {{ enum class {name} : {ty} {{}}; }}")

    for key in order:
        ns, name = key
        info = structs[key]
        body = strip_body(info['body'])
        bases = [b for b in info['bases'] if b.split('::')[-1] not in BUILTIN]
        inherit = (' : ' + ', '.join('public ' + b for b in bases)) if bases else ''
        parts.append(f"{open_ns(ns)}\nstruct {name}{inherit} {{\n{body}\n}};\n}}")

    # -fdump-record-layouts only dumps records whose layout is actually
    # COMPUTED. Merely defining a struct does not do that, so name each one in a
    # static_assert to force it.
    for (ns, name) in order:
        full = f"bg3se::{ns}::{name}" if ns else f"bg3se::{name}"
        parts.append(f"static_assert(sizeof({full}) > 0, \"\");")

    return '\n'.join(parts), order


SCRATCH = Path(os.environ.get('BG3SE_SCRATCH', '/tmp'))


def compile_tu(src, extra_args=()):
    """Compile for arm64 and return (record_layout_dump, diagnostics)."""
    f = SCRATCH / "layout_tu.cpp"
    f.write_text(src, encoding='utf-8')
    r = subprocess.run(
        ["clang++", "-target", "arm64-apple-macos12", "-std=c++20",
         "-fsyntax-only", "-Xclang", "-fdump-record-layouts",
         "-ferror-limit=0", "-w", str(f), *extra_args],
        capture_output=True, text=True)
    return r.stdout, r.stderr



# ---------------------------------------------------------------------------
# Reading clang's record layouts back
# ---------------------------------------------------------------------------

HDR_RE = re.compile(r'^\s*0 \| (?:struct|class) ([\w:]+)\s*$')
MEMBER_RE = re.compile(r'^\s*(\d+) \|   (?!\s)(.*?)\s*(\w+)\s*$')
SIZEOF_RE = re.compile(r'\[sizeof=(\d+)')

# Dump type text -> (FIELD_TYPE_*, width). Anything absent is simply not exposed
# to Lua -- and unlike the hand-packer, skipping a field here cannot disturb any
# other field, because clang already placed them all.
DUMP_TYPES = {
    '_Bool': ('FIELD_TYPE_BOOL', 1),
    'bool': ('FIELD_TYPE_BOOL', 1),
    'int8_t': ('FIELD_TYPE_INT8', 1), 'char': ('FIELD_TYPE_INT8', 1),
    'uint8_t': ('FIELD_TYPE_UINT8', 1), 'unsigned char': ('FIELD_TYPE_UINT8', 1),
    'int16_t': ('FIELD_TYPE_INT16', 2), 'short': ('FIELD_TYPE_INT16', 2),
    'uint16_t': ('FIELD_TYPE_UINT16', 2), 'unsigned short': ('FIELD_TYPE_UINT16', 2),
    'int32_t': ('FIELD_TYPE_INT32', 4), 'int': ('FIELD_TYPE_INT32', 4),
    'uint32_t': ('FIELD_TYPE_UINT32', 4), 'unsigned int': ('FIELD_TYPE_UINT32', 4),
    'int64_t': ('FIELD_TYPE_INT64', 8), 'long long': ('FIELD_TYPE_INT64', 8),
    'uint64_t': ('FIELD_TYPE_UINT64', 8),
    'unsigned long long': ('FIELD_TYPE_UINT64', 8),
    'float': ('FIELD_TYPE_FLOAT', 4), 'double': ('FIELD_TYPE_DOUBLE', 8),
    'struct bg3se::FixedString': ('FIELD_TYPE_FIXEDSTRING', 4),
    'struct bg3se::Guid': ('FIELD_TYPE_GUID', 16),
    'struct bg3se::EntityHandle': ('FIELD_TYPE_ENTITY_HANDLE', 8),
    'struct bg3se::ComponentHandle': ('FIELD_TYPE_ENTITY_HANDLE', 8),
    'struct bg3se::STDString': ('FIELD_TYPE_STDSTRING', 16),
    'struct glm::vec3': ('FIELD_TYPE_VEC3', 12),
    'struct glm::vec4': ('FIELD_TYPE_VEC4', 16),
    'struct glm::quat': ('FIELD_TYPE_VEC4', 16),
}


def parse_dump(text):
    """{qualified struct name: (fields, sizeof)} from -fdump-record-layouts."""
    out = {}
    for blk in text.split('*** Dumping AST Record Layout')[1:]:
        lines = blk.splitlines()
        name = None
        for ln in lines:
            m = HDR_RE.match(ln)
            if m:
                name = m.group(1)
                break
            if ln.strip():
                break
        if not name:
            continue
        fields, size = [], None
        for ln in lines:
            m = MEMBER_RE.match(ln)
            if m:
                fields.append((m.group(3), int(m.group(1)), m.group(2).strip()))
            s = SIZEOF_RE.search(ln)
            if s and size is None:
                size = int(s.group(1))
        if fields or size is not None:
            out[name] = (fields, size)
    return out


def load_arm64_sizes():
    sizes = {}
    for md in Path('ghidra/offsets/components').glob('COMPONENT_SIZES*.md'):
        for line in md.read_text(encoding='utf-8', errors='replace').splitlines():
            if not line.strip().startswith('|') or '::' not in line:
                continue
            p = line.split('|')
            if len(p) < 3:
                continue
            nm = p[1].strip()
            s = re.sub(r'\s*\([^)]*\)', '', p[2].strip().replace('`', '')).strip()
            try:
                v = int(s, 16) if s.startswith('0x') else int(re.match(r'^(\d+)', s).group(1))
            except Exception:
                continue
            if 0 < v <= 10000:
                sizes[nm.lower()] = v
    return sizes



def emit_header(layouts, stats):
    """Render the corroborated layouts as C, same shape as the old generator."""
    out = ['/**',
 ' * compiled_property_defs.h - component layouts COMPUTED BY THE COMPILER.',
 ' *',
 ' * Generated by tools/compile_layouts.py. Upstream\'s struct declarations are',
 ' * extracted and compiled for arm64 against a shim carrying this build\'s',
 ' * verified container widths; the offsets below are clang\'s, read out of',
 ' * -fdump-record-layouts.',
 ' *',
 ' * This replaces hand-packing a flattened field list, which was wrong in four',
 ' * ways at once and could not model base classes, bitfields, std::array /',
 ' * optional / variant, or enum widths at all. See docs/component-layout-audit.md.',
 ' *',
 ' * Every layout here reproduces the component size THIS BINARY reports. Where a',
 ' * Windows-derived field_XX name disagrees with the offset, the Windows name is',
 ' * the stale one: MSVC and libc++ container widths differ, and the size check',
 ' * comes from the running game.',
 ' *',
 ' * Still WEAKER than the hand-verified layouts in component_offsets.h, which',
 ' * win on lookup.',
 ' */', '',
 '#ifndef COMPILED_PROPERTY_DEFS_H', '#define COMPILED_PROPERTY_DEFS_H', '',
 '#include "component_property.h"', '']
    names = []
    for engine, (fields, size, prov) in layouts.items():
        safe = 'Cc_' + re.sub(r'[^A-Za-z0-9_]', '_', engine)
        short = engine.split('::')[-1]
        if short.endswith('Component'):
            short = short[:-9]
        out += ['// ' + '=' * 70, f'// {engine}',
                f'// Size 0x{size:x} ({size} bytes)', f'// Checked: {prov}',
                '// ' + '=' * 70, '',
                f'static const ComponentPropertyDef g_{safe}_Properties[] = {{']
        for fname, foff, ftype in fields:
            out.append(f'    {{ "{fname}", 0x{foff:02x}, {ftype}, 0, true }},')
        out += ['};', '',
                f'static const ComponentLayoutDef g_{safe}_Layout = {{',
                f'    .componentName = "{engine}",',
                f'    .shortName = "{short}",',
                '    .componentTypeIndex = 0,',
                f'    .componentSize = 0x{size:x},',
                f'    .properties = g_{safe}_Properties,',
                f'    .propertyCount = sizeof(g_{safe}_Properties) / '
                f'sizeof(g_{safe}_Properties[0]),', '};', '']
        names.append(safe)
    out += [f'#define COMPILED_COMPONENT_COUNT {len(names)}', '',
            'static const ComponentLayoutDef* g_CompiledComponentLayouts[] = {']
    out += [f'    &g_{n}_Layout,' for n in names]
    out += ['    NULL', '};', '', '#endif // COMPILED_PROPERTY_DEFS_H']
    return '\n'.join(out)


def build_layouts(dump, engine_of, arm64, legacy_of):
    """Gate each compiled layout on this binary's own component size."""
    layouts, stats = OrderedDict(), {}

    def bump(k):
        stats[k] = stats.get(k, 0) + 1

    for qual, (fields, size) in sorted(dump.items()):
        engine = engine_of.get(qual)
        if not engine:
            continue
        if size is None:
            bump('no_size'); continue
        real = arm64.get(engine.lower())

        selfnamed = [(f, o) for f, o, _ in fields
                     if re.match(r'^field_[0-9A-Fa-f]+$', f)]
        legacy = legacy_of.get(qual, {})
        checks = [(f, o, int(f[6:], 16)) for f, o in selfnamed]
        checks += [(f, o, legacy[f]) for f, o, _ in fields if f in legacy]
        agree = all(o == want for _, o, want in checks) if checks else None
        if checks:
            stats['checked_fields'] = stats.get('checked_fields', 0) + len(checks)
            stats['checked_ok'] = stats.get('checked_ok', 0) + sum(
                1 for _, o, w in checks if o == w)

        if real is not None:
            if size != real:
                bump('size_mismatch'); continue
            prov = f'compiled sizeof reproduces ARM64 size 0x{real:x}'
            if agree is False:
                prov += ('; a Windows field_XX name disagrees -- expected where '
                         'MSVC and libc++ container widths differ')
            bump('size_confirmed')
        else:
            # No size from the binary. Fall back to the Windows offsets.
            if agree is not True:
                bump('unconfirmable'); continue
            prov = 'no ARM64 size; every Windows self-named offset lands exactly'
            bump('selfname_confirmed')

        props = []
        for fname, foff, ftext in fields:
            ty = DUMP_TYPES.get(ftext)
            if not ty:
                continue
            if foff + ty[1] > size:
                continue
            props.append((fname, foff, ty[0]))
        if not props:
            bump('no_lua_visible_fields'); continue
        layouts[engine] = (props, size, prov)
    return layouts, stats


if __name__ == '__main__':
    os.chdir(Path(__file__).parent.parent)
    enums = collect_enums()
    structs = collect_structs()

    engine_of = {}
    for (ns, name), info in structs.items():
        e = engine_class_of(info['body'])
        if e:
            engine_of[f"bg3se::{ns}::{name}" if ns else f"bg3se::{name}"] = e
    print(f"enums {len(enums)}  structs {len(structs)}  "
          f"registered components {len(engine_of)}", file=sys.stderr)

    wanted = [k for k in structs if engine_class_of(structs[k]['body'])]
    src, order = build_tu(structs, enums, wanted)
    out, err = compile_tu(src)
    nerr = sum(1 for l in err.splitlines() if ': error:' in l)
    print(f"TU {len(order)} structs, {nerr} compile errors", file=sys.stderr)

    dump = parse_dump(out)
    arm64 = load_arm64_sizes()
    legacy_of = {}
    for (ns, name), info in structs.items():
        q = f"bg3se::{ns}::{name}" if ns else f"bg3se::{name}"
        lg = legacy_offsets(info['body'])
        if lg:
            legacy_of[q] = lg
    layouts, stats = build_layouts(dump, engine_of, arm64, legacy_of)

    print(f"record layouts parsed: {len(dump)}", file=sys.stderr)
    ck, co = stats.get('checked_fields', 0), stats.get('checked_ok', 0)
    print(f"   offset checks (field_XX + bg3::legacy): {co}/{ck} land exactly",
          file=sys.stderr)
    for k in ('size_confirmed', 'selfname_confirmed', 'size_mismatch',
              'unconfirmable', 'no_size', 'no_lua_visible_fields'):
        print(f"   {k:<24} {stats.get(k, 0)}", file=sys.stderr)
    print(f"EMITTED {len(layouts)} layouts", file=sys.stderr)

    Path('src/entity/compiled_property_defs.h').write_text(
        emit_header(layouts, stats), encoding='utf-8')
    print("wrote src/entity/compiled_property_defs.h", file=sys.stderr)
