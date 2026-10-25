// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>

#include "types.h"

// How a character is drawn.
enum class GdxsvFontStyle {
	// DC2's text: GDXGothic Medium with the game's own edge, a rim the text colour tints darker.
	Text,
	// The in-battle pilot names (HUD, name tags, INFORMATION panel): smaller, with a see-through dark outline.
	BattleName,
	// A cell of DC2's battle-message sheet (the INFORMATION panel's messages, beside a pilot name): BattleName at the
	// same size on screen, in a 20x20 texel image.
	BattleMessage,
};

// Replacement textures for DC2's 24 px font glyphs, drawn from the bundled GDXGothic Medium (gdxsv:FontRaster).
// GDXGothic's em is the game's 24 px cell, with the game font's sizes and placement built in: a character is drawn
// with the pen at the cell's left edge. A game glyph texture is known by its hash (gdxsv_font_glyphs.inc).
// The font also has every JIS X 0208 and Big5-HKSCS character: RenderCodePoint draws a character the game font
// lacks in the same cell, for text that carries Unicode (lobby text, pilot names).
class GdxsvFontProvider {
   public:
	GdxsvFontProvider();
	~GdxsvFontProvider();
	bool HasGlyph(u32 hash) const;
	// A malloc'ed RGBA image of the glyph texture with this hash (32x32 game px, or 20x20 texels for BattleMessage, at
	// 4x, rows top-down like the texture), or nullptr. Thread-safe.
	u8* Render(u32 hash, GdxsvFontStyle style, int& width, int& height);
	// The same for any character of the font. These load the font.
	bool HasCodePoint(u32 code_point);
	u8* RenderCodePoint(u32 code_point, GdxsvFontStyle style, int& width, int& height);
	// Frees the font and the cache.
	void Close();

   private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
