"""gdxsv defaults on a new config file (gdxsv_apply_base_settings, called by config::open);
settings read from an older release's emu.cfg."""

import re

from lib.case import case, result

# (flycast.config namespace, option, gdxsv default, stock flycast default)
BASE = [
    ("video", "RenderResolution", "720", "480"),
    ("video", "DelayFrameSwapping", "false", "true"),
    ("video", "PerStripSorting", "false", "false"),
    ("video", "SkipFrame", "0", "0"),
    ("video", "AutoSkipFrame", "0", "0"),
    ("video", "TextureUpscale", "1", "1"),
    ("audio", "DSPEnabled", "false", "false"),
    ("audio", "AudioVolume", "50", "100"),
    ("audio", "AudioBufferSize", "2824", "2822"),
    ("advanced", "DynarecEnabled", "true", "true"),
]


def base_settings(ctx, name: str, emu_cfg) -> list:
    p = ctx.probe(name, ctx.rom("dc2"), "config.lua",
                  {"frame": 5, "options": [[ns, o] for ns, o, _, _ in BASE]}, frames=20, emu_cfg=emu_cfg,
                  muted=False)
    checks = [{"what": "probe ran", "ok": p.clean, **p.facts()}]
    for ns, o, want, stock in BASE:
        got = p.e2e.get(f"{ns}.{o}")
        checks.append({"what": f"{ns}.{o}", "want": want, "stock": stock, "got": got, "ok": got == want})
    return checks


@case("C1", "gdxsv defaults on a new config file: video, audio and CPU settings of gdxsv_apply_base_settings",
      tier="smoke")
def config_defaults(ctx):
    return result(base_settings(ctx, "C1", None))


# An emu.cfg with the section and key names gdxsv-1.9.6 writes (its gdxsv options lived in
# core/cfg/option.cpp then), each set to a value that is the default in no build.
OLD_CFG = """[config]
rend.Resolution = 960
aica.Volume = 30

[gdxsv]
language = 1
loginkey = E2ECFG0001
LocalPort = 29871
"""
OLD_LUA = [("video", "RenderResolution", "960"), ("audio", "AudioVolume", "30")]
OLD_LOG = [("gdxsv.loginkey", r"loginkey:(\S+)", "E2ECFG0001"), ("gdxsv.LocalPort", r"udp_port:(\d+)", "29871"),
           ("gdxsv.language", r"lang_patch id=\S+ prev=\S+ lang=(\d+)", "1")]


def old_config(ctx, name: str, emu_cfg: str) -> list:
    p = ctx.probe(name, ctx.rom("dc2"), "config.lua",
                  {"frame": 30, "options": [[ns, o] for ns, o, _ in OLD_LUA]}, frames=60, emu_cfg=emu_cfg,
                  muted=False)
    checks = [{"what": "probe ran", "ok": p.clean, **p.facts()}]
    for ns, o, want in OLD_LUA:
        got = p.e2e.get(f"{ns}.{o}")
        checks.append({"what": f"{ns}.{o}", "want": want, "got": got, "ok": got == want})
    for what, pattern, want in OLD_LOG:
        m = re.search(pattern, p.log)
        got = m.group(1) if m else None
        checks.append({"what": what, "want": want, "got": got, "ok": got == want})
    return checks


@case("C2", "an emu.cfg in gdxsv-1.9.6's layout: its flycast and gdxsv values hold after the game start",
      tier="smoke")
def config_old(ctx):
    return result(old_config(ctx, "C2", OLD_CFG))


# The socket binds only the low 16 bits (70000 -> 4464, -5 -> 65531) while LBS is told the full value.
BAD_PORTS = ["70000", "-5"]


def local_port(ctx, name: str, ports: list) -> list:
    checks = []
    for port in ports:
        p = ctx.probe(f"{name}_{port}", ctx.rom("dc2"), "memread.lua", {"frame": 5, "reads": []}, frames=20,
                      emu_cfg=f"[gdxsv]\nLocalPort = {port}\n")
        m = re.search(r"udp_port:(-?\d+)", p.log)
        got = int(m.group(1)) if m else None
        checks.append({"what": f"LocalPort {port}: replaced by a port in 1-65535", "got": got,
                       "ok": p.clean and got is not None and got != int(port) and 1 <= got <= 65535, **p.facts()})
    return checks


@case("C3", "an emu.cfg LocalPort a UDP socket cannot bind (70000, -5): gdxsv uses a port in 1-65535", tier="smoke")
def config_local_port(ctx):
    return result(local_port(ctx, "C3", BAD_PORTS))
