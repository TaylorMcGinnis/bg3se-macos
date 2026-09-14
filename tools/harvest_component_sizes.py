#!/usr/bin/env python3
"""
Harvest EntityStorageData::ComponentSizes from a running game.

The layout generators gate every emitted layout on a component size, because a
layout whose computed size disagrees with the real one is wrong somewhere and
must not ship. That gate is only as good as the size table behind it, and the
static one extracted from Ghidra is both incomplete (143 sound compiled layouts
are dropped for want of an entry) and in places wrong (ls::EffectComponent is
recorded as 0x8, smaller than its own first two fields).

The running game holds the authoritative table -- it is the stride the ECS
actually uses. Ext.Entity.GetComponentSizes() reads it.

Usage, with the game running and a save loaded:

    python3 tools/harvest_component_sizes.py

Only components some loaded storage class carries are reported, so a bigger and
more varied save yields a better table. Runs are merged into the existing file
rather than replacing it, so harvesting from several saves accumulates.
"""

import json
import re
import subprocess
import sys
from pathlib import Path

OUT = Path('ghidra/offsets/components/live_component_sizes.json')

LUA = '''
if type(Ext.Entity.GetComponentSizes) ~= "function" then
    _P("HARVEST_UNAVAILABLE")
    return
end
local t = Ext.Entity.GetComponentSizes()
local lines = {}
for k, v in pairs(t) do lines[#lines+1] = k .. "=" .. v end
table.sort(lines)
_P("HARVEST_BEGIN " .. #lines)
for _, l in ipairs(lines) do _P(l) end
_P("HARVEST_END")
'''


def main():
    root = Path(__file__).resolve().parent.parent
    script = root / 'build' / 'harvest_sizes.lua'
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text(LUA, encoding='utf-8')

    r = subprocess.run([sys.executable, '-m', 'bg3se_harness', 'eval', str(script)],
                       capture_output=True, text=True, cwd=root,
                       env={**__import__('os').environ, 'PYTHONPATH': 'tools'})
    out = r.stdout + r.stderr
    if 'HARVEST_UNAVAILABLE' in out:
        print("Ext.Entity.GetComponentSizes is not present: the running game is "
              "on an older dylib. Relaunch after a build, load a save, retry.",
              file=sys.stderr)
        return 1
    if 'HARVEST_BEGIN' not in out:
        print("no response from the game; is it running with a save loaded?",
              file=sys.stderr)
        print(out[-500:], file=sys.stderr)
        return 1

    sizes = {}
    for m in re.finditer(r'^([\w:]+)=(\d+)$', out, re.M):
        sizes[m.group(1)] = int(m.group(2))
    if not sizes:
        print("harvest returned no entries", file=sys.stderr)
        return 1

    merged, added, conflicts = {}, 0, []
    path = root / OUT
    if path.exists():
        merged = json.loads(path.read_text(encoding='utf-8'))
    for k, v in sizes.items():
        if k in merged and merged[k] != v:
            conflicts.append((k, merged[k], v))
        elif k not in merged:
            added += 1
        merged[k] = v

    path.write_text(json.dumps(merged, indent=1, sort_keys=True), encoding='utf-8')
    print(f"harvested {len(sizes)} sizes; {added} new, {len(merged)} total")
    for k, a, b in conflicts[:10]:
        print(f"  CONFLICT {k}: had {a}, now {b}")
    if conflicts:
        print(f"  ({len(conflicts)} conflicts -- the engine reported a different "
              f"size than a previous run, which should not happen for the same "
              f"build; check the game version)")
    return 0


if __name__ == '__main__':
    sys.exit(main())
