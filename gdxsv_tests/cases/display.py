"""Translation patch, widescreen HUD."""

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
