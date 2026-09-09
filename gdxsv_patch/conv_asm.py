#!/usr/bin/env python

"""
Convert asm into c++ (flycast codes).
"""

import sys
import re
import time
from typing import NamedTuple

r_ope = re.compile(r"^\s*?([0-9a-f]+):\s*([0-9a-f]+) ([0-9a-f]+)\s*(.*)$")
r_symbol = re.compile(r"^([0-9a-f]+) <(\w+)>:")
r_section = re.compile(r"^Disassembly of section ([0-9a-zA-Z._]+):")
section = None
start = False

f = open('../core/gdxsv/gdxsv_patch.inc', 'w')
for line in open('bin/gdxsv_patch.asm'):
    line = line.rstrip()
    if 'Disassembly' in line:
        start = True
    if not start:
        continue

    g = r_section.match(line)
    if g:
        section = g.group(1).strip()
        print("section", section)
        f.write(f"//\n")
        f.write(f"// section {section}\n")
        f.write(f"//\n")

    g = r_ope.match(line)
    if g:
        addr = int(g.group(1), 16)
        data = int(g.group(2), 16) | int(g.group(3), 16) << 8
        if section == "gdx.func":
            addr += 0x80000000
        f.write(f"gdxsv_WriteMem16(0x{addr:08x}u, 0x{data:04x}u); // {g.group(4)}\n")

    g = r_symbol.match(line)
    if g:
        addr = int(g.group(1), 16)
        name = g.group(2)
        if section in ("gdx.data", "gdx.func"):
            f.write(f'symbols_["{name}"] = 0x{addr:08x};\n')

f.write(f'if (disk_ == 1) gdxsv_WriteMem32(0x8c181bb4, symbols_["gdx_dial_start_disk1"]);\n')
f.write(f'if (disk_ == 2) gdxsv_WriteMem32(0x8c1e0274, symbols_["gdx_dial_start_disk2"]);\n')
# Slot 99 bypasses dialing. Publish complete-message hooks with the payload,
# after its code/data are present. Guest initialization owns the display hooks
# and records; same-version savestates retain their saved pending requests.
stats_hooks = [
    (0x0c030b04, 0x0c036094, "gdx_player_info32_request"),
    (0x0c02cf44, 0x0c034e9c, "gdx_win_lose32_request"),
    (0x0c03224c, 0x0c034e9c, "gdx_win_lose32_request"),
    (0x0c030aec, 0x0c033e20, "gdx_stats_poll"),
    (0x0c02ccbc, 0x0c033e20, "gdx_stats_poll"),
    (0x0c03221c, 0x0c033e20, "gdx_stats_poll"),
]
draw_hooks = [
    (0x0c03e454, 0x0c02404c, "gdx_player_info32_draw"),
    (0x0c041f8c, 0x0c02404c, "gdx_win_lose32_draw"),
]
f.write('if (disk_ == 2) {\n')
f.write('    // Install all stats entry points together, only on the known ROM layout.\n')
checks = [f'(gdxsv_ReadMem32(0x{cell:08x}) == 0x{original:08x} || '
          f'gdxsv_ReadMem32(0x{cell:08x}) == symbols_["{name}"])'
          for cell, original, name in stats_hooks + draw_hooks]
f.write('    if (' + '\n        && '.join(checks) + ') {\n')
for cell, _, name in stats_hooks:
    f.write(f'        gdxsv_WriteMem32(0x{cell:08x}, symbols_["{name}"]);\n')
f.write('    } else {\n')
f.write('        WARN_LOG(COMMON, "Stats hooks: unexpected guest pointers; keeping the legacy protocol");\n')
f.write('    }\n}\n')
f.write(f'symbols_[":patch_id"] = {str(int(time.time()) % 100000000)};\n')
f.write(f'gdxsv_WriteMem32(symbols_["patch_id"], symbols_[":patch_id"]);\n')
f.close()
