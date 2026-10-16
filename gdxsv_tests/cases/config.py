"""gdxsv defaults on a new config file (gdxsv_apply_base_settings, called by config::open)."""

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
                  {"frame": 5, "options": [[ns, o] for ns, o, _, _ in BASE]}, frames=20, emu_cfg=emu_cfg)
    checks = [{"what": "probe ran", "ok": p.clean, **p.facts()}]
    for ns, o, want, stock in BASE:
        got = p.e2e.get(f"{ns}.{o}")
        checks.append({"what": f"{ns}.{o}", "want": want, "stock": stock, "got": got, "ok": got == want})
    return checks


@case("C1", "gdxsv defaults on a new config file: video, audio and CPU settings of gdxsv_apply_base_settings",
      tier="smoke")
def config_defaults(ctx):
    return result(base_settings(ctx, "C1", None))


@case("C1c", "C1's checks with an emu.cfg already there (empty): the stock values show", control_of="C1")
def config_defaults_control(ctx):
    checks = base_settings(ctx, "C1c", "")
    differ = [c for c in checks[1:] if c["want"] != c["stock"]]
    return result([checks[0],
                   {"what": "every gdxsv default that differs from stock fails the check",
                    "failed": [c["what"] for c in differ if not c["ok"]],
                    "ok": all(not c["ok"] for c in differ)}], detail=checks)
