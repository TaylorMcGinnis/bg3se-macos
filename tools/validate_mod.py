#!/usr/bin/env python3
"""
Report every GUID a mod references that nothing defines.

Why this exists
---------------
A custom origin mod crashed character creation three times in a row. Each time
the cause was a GUID pointing at something that does not exist, and each time it
was found by hand, one per launch: first the class/subclass/race on the origin,
then a schema mismatch in the appearance visuals, finally the root template's
parent. Checking every reference at once takes about as long as guessing at one
of them, and it answers the question completely.

A dangling GUID is not a soft failure. The engine looks it up, gets null, and
dereferences it -- `ecl::character_creation::ProcessCommands` faults at +0x1c
with no log line and no Lua error, because nothing in the extender is involved.

What it checks
--------------
Every attribute that must resolve to a definition (REF_ATTRS) against:
  1. what the mod itself defines, and
  2. what the game defines, across the paks that carry definitions.

GUIDs are stored as ASCII in both `.lsx` and the game's binary `.lsf` banks, so
a text scan finds them. That was verified with a control -- a known-good GUID
from the bank is found by this method -- before trusting any negative result,
because "not found" is only meaningful if the search can find things at all.

Usage
-----
    python3 tools/validate_mod.py <mod.pak | mod-directory> [--refresh]

The game's GUID index is cached after the first run (extracting the definition
paks takes a while); --refresh rebuilds it after a game update.
"""

import argparse
import gzip
import os
import re
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

GUID_RE = r'[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}'
NULL_GUID = '00000000-0000-0000-0000-000000000000'

# Attributes whose value must name something that exists. Anything not listed
# here is ignored, because dialogs and timelines are full of internal node ids
# that are self-referential and resolve to nothing by design -- scanning every
# GUID indiscriminately reported 70 "problems" for a mod that had 2.
REF_ATTRS = (
    'ClassUUID', 'SubClassUUID', 'RaceUUID', 'SubRaceUUID', 'BackgroundUUID',
    'GodUUID', 'GlobalTemplate', 'VoiceTableUUID', 'IntroDialogUUID',
    'TemplateName', 'ParentTemplateId', 'RootTemplate', 'VisualResource',
    'HeadAppearanceUUID', 'DefaultSkinColor', 'Object', 'ProgressionTableUUID',
    'EquipmentSetUUID', 'TableUUID', 'ProgressionUUID',
)

# Attributes that DEFINE a GUID.
DEF_ATTRS = ('UUID', 'MapKey', 'ID')

# Paks that carry definitions. Kept deliberately broad: a definition source we
# forget to read shows up as a false "unresolved", which is the failure mode
# that wastes someone's evening.
DEFINITION_PAKS = ('Shared.pak', 'Gustav.pak', 'GustavX.pak', 'Honour.pak',
                   'HonourX.pak', 'Engine.pak', 'Game.pak')

GAME_DATA = ("~/Library/Application Support/Steam/steamapps/common/"
             "Baldurs Gate 3/Baldur's Gate 3.app/Contents/Data")


def cache_path() -> Path:
    return Path(__file__).resolve().parent.parent / 'build' / 'game_guid_index.txt.gz'


def extract_pak(pak: Path, dest: Path) -> bool:
    tool = Path(__file__).resolve().parent / 'extract_pak.py'
    r = subprocess.run([sys.executable, str(tool), str(pak), str(dest)],
                       capture_output=True, text=True)
    return r.returncode == 0


def guids_in_tree(root: Path, bare: bool):
    """Every GUID under `root`. `bare` also takes unattributed ASCII GUIDs,
    which is how the binary .lsf banks store them."""
    found = set()
    for dirpath, _, files in os.walk(root):
        for name in files:
            path = os.path.join(dirpath, name)
            try:
                text = open(path, 'rb').read().decode('utf-8', 'replace')
            except Exception:
                continue
            if bare:
                found.update(m.group(0).lower() for m in re.finditer(GUID_RE, text))
            else:
                for attr in DEF_ATTRS:
                    pat = r'id="%s"[^>]*value="(%s)"' % (attr, GUID_RE)
                    found.update(m.group(1).lower() for m in re.finditer(pat, text))
    return found


def build_game_index(refresh: bool) -> set:
    cache = cache_path()
    if cache.exists() and not refresh:
        with gzip.open(cache, 'rt') as fh:
            return {line.strip() for line in fh if line.strip()}

    data = Path(os.path.expanduser(GAME_DATA))
    if not data.is_dir():
        sys.exit(f"game data not found at {data}")

    print("Building the game GUID index (first run only; --refresh to rebuild)…",
          file=sys.stderr)
    guids = set()
    with tempfile.TemporaryDirectory() as tmp:
        for pak in DEFINITION_PAKS:
            src = data / pak
            if not src.exists():
                continue
            dest = Path(tmp) / pak.replace('.pak', '')
            dest.mkdir(parents=True, exist_ok=True)
            print(f"   {pak}…", file=sys.stderr)
            if extract_pak(src, dest):
                guids |= guids_in_tree(dest, bare=True)

    cache.parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(cache, 'wt') as fh:
        for g in sorted(guids):
            fh.write(g + '\n')
    print(f"   indexed {len(guids):,} GUIDs -> {cache}", file=sys.stderr)
    return guids


def scan_mod(root: Path):
    """Returns (defined, refs, unreadable).

    `unreadable` is the binary .lsf files we cannot inspect. A GUID inside a
    binary cannot be attributed to an attribute, so references and definitions
    are indistinguishable there and scanning it would produce noise rather than
    findings. The count is reported instead of ignored, because a validator that
    prints "all clear" for files it never opened is worse than one that says
    nothing -- it converts unknown into safe."""
    defined = set()
    refs = defaultdict(set)
    unreadable = []
    for dirpath, _, files in os.walk(root):
        for name in files:
            path = os.path.join(dirpath, name)
            if name.endswith('.lsf'):
                unreadable.append(os.path.relpath(path, root))
                continue
            if not name.endswith(('.lsx', '.xml', '.txt')):
                continue
            try:
                text = open(path, 'rb').read().decode('utf-8', 'replace')
            except Exception:
                continue
            rel = os.path.relpath(path, root)
            for attr in DEF_ATTRS:
                pat = r'id="%s"[^>]*value="(%s)"' % (attr, GUID_RE)
                defined.update(m.group(1).lower() for m in re.finditer(pat, text))
            for attr in REF_ATTRS:
                pat = r'id="%s"[^>]*value="(%s)"' % (attr, GUID_RE)
                for m in re.finditer(pat, text):
                    refs[m.group(1).lower()].add((rel, attr))
    return defined, refs, unreadable


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mod', help='.pak file or an already-extracted mod directory')
    ap.add_argument('--refresh', action='store_true',
                    help='rebuild the game GUID index (after a game update)')
    args = ap.parse_args()

    game = build_game_index(args.refresh)

    mod_path = Path(os.path.expanduser(args.mod))
    tmp = None
    if mod_path.is_file():
        tmp = tempfile.TemporaryDirectory()
        root = Path(tmp.name)
        if not extract_pak(mod_path, root):
            sys.exit(f"could not extract {mod_path}")
    else:
        root = mod_path

    defined, refs, unreadable = scan_mod(root)
    uses = sum(len(v) for v in refs.values())
    print(f"{mod_path.name}: defines {len(defined)} GUIDs, "
          f"makes {uses} referencing uses of {len(refs)} distinct GUIDs")
    print(f"game index: {len(game):,} GUIDs")
    if unreadable:
        print(f"\n  NOTE: {len(unreadable)} binary .lsf file(s) were NOT checked -- "
              f"references inside them are invisible to this tool.")
        for f in unreadable[:4]:
            print(f"        {f}")
        if len(unreadable) > 4:
            print(f"        … and {len(unreadable) - 4} more")
        print("        Convert them to .lsx to include them in the check.")
    print()

    bad = {g: v for g, v in refs.items()
           if g != NULL_GUID and g not in defined and g not in game}
    if not bad:
        if refs:
            print("All referenced GUIDs resolve.")
        else:
            print("Nothing to check: no referencing attributes found in readable "
                  "files. This is NOT a clean bill of health.")
        return 0

    print(f"UNRESOLVED ({len(bad)}) — the engine will read null for each of these:\n")
    for guid, where in sorted(bad.items(), key=lambda kv: -len(kv[1])):
        attrs = sorted({a for _, a in where})
        files = sorted({f for f, _ in where})
        print(f"  {guid}")
        print(f"      as {', '.join(attrs)}")
        for f in files[:4]:
            print(f"      in {f}")
        print()
    return 1


if __name__ == '__main__':
    sys.exit(main())
