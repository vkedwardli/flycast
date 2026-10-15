"""Translation patch, widescreen HUD (judged by RAM)."""

import os
import re
import struct

from lib.case import case, result

# gdxsv:language (core/gdxsv/gdxsv_translation.h GdxsvLanguage) and the first translated
# string (0x8c010000 + game offset 0x1ba264) as each language leaves it. Japanese is the
# game's own text, the same as disabled.
TEXT = 0x8c1ca264
LANG_TEXT = {
    "japanese": (0, "8349815b8367838d815b8368928682c5"),
    "cantonese": (1, "90b38ddde6a48ee691b69e8381458145"),
    "english": (2, "4c6f6164696e6720736176652066696c"),  # "Loading save fil"
    "disabled": (3, "8349815b8367838d815b8368928682c5"),
}


def lang_text(ctx, lang: str, want: str) -> list:
    p = ctx.probe(lang, ctx.rom("dc2"), "memread.lua", {"frame": 30, "reads": [[TEXT, 16]]}, frames=50,
                  config=[f"gdxsv:language={LANG_TEXT[lang][0]}"])
    got = p.e2e.get(f"0x{TEXT:08x}:16")
    return [{"language": lang, "what": "probe ran", "ok": p.clean, **p.facts()},
            {"language": lang, "want": want, "got": got, "ok": got == LANG_TEXT[want][1]}]


@case("L1", "translation patch: Disc 2 text per language", tier="smoke")
def translation(ctx):
    return result(sum((lang_text(ctx, lang, lang) for lang in LANG_TEXT), []))


@case("L1c", "L1 on language=disabled with the English expectation", control_of="L1")
def translation_control(ctx):
    ran, text = lang_text(ctx, "disabled", "english")
    return result([ran, {"what": "English text check fails", "ok": not text["ok"]}], detail=text)


SYMBOLS = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                       "core", "gdxsv", "gdxsv_patch.inc")
WS_SYMBOLS = ["gdx_widescreen_transition_left_x", "gdx_widescreen_transition_right_x",
              "gdx_widescreen_hud_right_offset"]


def symbol(name: str) -> int:
    with open(SYMBOLS, encoding="utf-8") as f:
        m = re.search(r'symbols_\["%s"\] = (0x[0-9a-f]+);' % re.escape(name), f.read())
    return int(m.group(1), 16)


def ws_expected(wide: bool, super_wide: bool, layout: int, w: int, h: int) -> list:
    """Gdxsv::WriteWidescreenPatch: left/right edge of the transition matte, HUD offset."""
    f32 = lambda x: struct.unpack("<f", struct.pack("<f", x))[0]  # noqa: E731
    stock, view = 4 / 3, f32(w / h)
    aspect = hud = stock
    if wide:
        aspect = view if super_wide else 16 / 9
        if layout == 1:
            hud = max(stock, min(view, 16 / 9))
        elif layout == 2:
            hud = max(stock, min(view, aspect))
    scale = aspect / stock
    return [320 - 325 * scale if wide else 0.0, 320 + 325 * scale if wide else 640.0, 0.075 * (hud - stock)]


# (name, widescreen, super widescreen, HUD layout, viewport)
WS_CASES = [
    ("off", False, False, 1, 854, 480),
    ("layout0_16x9", True, False, 0, 854, 480),
    ("layout1_16x9", True, False, 1, 854, 480),
    ("layout2_16x9", True, False, 2, 854, 480),
    ("layout1_4x3", True, False, 1, 640, 480),
    ("super_layout2_21x9", True, True, 2, 1120, 480),
]


def ws_run(ctx, name, wide, super_wide, layout, w, h, expect) -> list:
    yn = lambda b: "yes" if b else "no"  # noqa: E731
    addrs = [symbol(s) for s in WS_SYMBOLS]
    p = ctx.probe(name, ctx.rom("dc2"), "memread.lua", {"frame": 30, "reads": [[a, 4] for a in addrs]}, frames=50,
                  config=[f"config:rend.WideScreen={yn(wide)}", f"config:rend.WidescreenGameHacks={yn(wide)}",
                          f"config:rend.SuperWideScreen={yn(super_wide)}", f"gdxsv:WidescreenHudLayout={layout}",
                          f"gdxsv:headless_width={w}", f"gdxsv:headless_height={h}"])
    got = []
    for a in addrs:
        v = p.e2e.get(f"0x{a:08x}:4")
        got.append(struct.unpack("<f", struct.pack("<I", int(v)))[0] if v else None)
    ok = all(g is not None and abs(g - e) < 1e-3 for g, e in zip(got, expect))
    return [{"case": name, "what": "probe ran", "ok": p.clean, **p.facts()},
            {"case": name, "want": [round(x, 4) for x in expect],
             "got": [g if g is None else round(g, 4) for g in got], "ok": ok}]


@case("W1", "widescreen HUD layout: matte edges and HUD offset per layout and viewport", tier="smoke")
def widescreen(ctx):
    return result(sum((ws_run(ctx, *c, expect=ws_expected(*c[1:])) for c in WS_CASES), []))


@case("W1c", "W1 with widescreen on, checked against the stock values", control_of="W1")
def widescreen_control(ctx):
    c = WS_CASES[3]
    ran, values = ws_run(ctx, *c, expect=ws_expected(False, *c[2:]))
    return result([ran, {"what": "stock values check fails", "ok": not values["ok"]}], detail=values)


@case("W0", "measure: W1 layout1_16x9 with extra config from W0_CONFIG")
def ws_measure(ctx):
    c = WS_CASES[2]
    extra = [x for x in os.environ.get("W0_CONFIG", "").split(";") if x]
    yn = lambda b: "yes" if b else "no"  # noqa: E731
    addrs = [symbol(s) for s in WS_SYMBOLS]
    p = ctx.probe("m", ctx.rom("dc2"), "memread.lua", {"frame": int(os.environ.get("W0_FRAME", "30")),
                  "reads": [[a, 4] for a in addrs]}, frames=int(os.environ.get("W0_FRAME", "30")) + 20,
                  config=["config:rend.WideScreen=yes", "config:rend.WidescreenGameHacks=yes",
                          "gdxsv:headless_width=854", "gdxsv:headless_height=480"] + extra,
                 window=bool(os.environ.get("W0_WINDOW")), loadstate=not os.environ.get("W0_WINDOW"), timeout=300)
    return result([{"ok": True}], e2e=p.e2e, facts=p.facts())
