// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// gdxsv:FontRaster's texture source: DC2's glyph textures and its battle-message sheet drawn from GDXGothic.

#include <chrono>
#include <cstdlib>
#include <cstring>

#include "gdxsv_custom_texture_source.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/pvr_regs.h"
#include "log/Log.h"

namespace {

// DC2's battle-message sheet: 256x256 texels, a grid of 20-texel cells holding the characters the game builds the
// INFORMATION panel's messages (命中, 撃破, 地球連邦軍危険...) and the CPU partner's orders from, and the two army
// emblems. The game shows these messages in Japanese whatever the translation.
constexpr u32 kMessageSheetHash = 0xeb90a6d4;
constexpr int kSheetTexels = 256;
constexpr int kSheetCell = 20;
constexpr int kOutScale = 4;  // output px per texel
constexpr int kOut = kSheetTexels * kOutScale;
// Its characters, by row of cells from the top; a space keeps the game's cell (emblems, empty cells). The 16-texel
// column on the right and the row at the bottom (small marks, COMMAND and PLAYER) keep the game's texels too.
const char32_t* const kSheetRows[] = {
	U"アイウエオカキクケコ赤い", U"サシスセソタチツテト彗星", U"ナニヌネノハヒフヘホ黒三", U"マミムメモヤユヨワヲ連邦",
	U"ンー・司令艦長提督大中少", U"佐尉将一二等兵上軍曹白の", U"ラリルレロ拡撃れて戦下陸", U"さよろし命中くんすちなた",
	U"かま損傷軽微破持 危険敵",	 U"射重視格闘援護避もう％．", U"機隻台回味方旧型ＧＭ  ",   U"地球に公国伍基せ ＣＰＵ",
};
constexpr int kSheetRowCount = sizeof(kSheetRows) / sizeof(kSheetRows[0]);

// A cell the font draws: not a space
bool is_char_cell(int r, int c) { return kSheetRows[r][c] != U' '; }

// The sheet's characters drawn in the BattleMessage style at kOutScale, rows bottom-up like the images of the other
// texture sources; the other cells are left clear.
void draw_message_chars(GdxsvFontProvider& font, std::vector<u8>& sheet) {
	const int cell = kSheetCell * kOutScale;
	sheet.assign((size_t)kOut * kOut * 4, 0);
	for (int r = 0; r < kSheetRowCount; r++)
		for (int c = 0; kSheetRows[r][c] != 0; c++) {
			if (!is_char_cell(r, c)) continue;
			int gw, gh;
			u8* glyph = font.RenderCodePoint(kSheetRows[r][c], GdxsvFontStyle::BattleMessage, gw, gh);
			if (glyph != nullptr && gw == cell && gh == cell)
				for (int y = 0; y < cell; y++)
					memcpy(&sheet[((size_t)(kOut - 1 - (r * cell + y)) * kOut + c * cell) * 4], glyph + (size_t)y * cell * 4, cell * 4);
			std::free(glyph);
		}
}

// Texel index of (x, y) in a twiddled w x h texture: the bits of y and x interleaved from the lowest, y first, while
// both last (texconv.cpp's twiddle_slow)
u32 twiddled(u32 x, u32 y, u32 w, u32 h) {
	u32 r = 0, sh = 0;
	for (w >>= 1, h >>= 1; w != 0 || h != 0;) {
		if (h != 0) {
			r |= (y & 1) << sh++;
			y >>= 1;
			h >>= 1;
		}
		if (w != 0) {
			r |= (x & 1) << sh++;
			x >>= 1;
			w >>= 1;
		}
	}
	return r;
}

// A palette entry as RGBA, in the palette RAM's format (PAL_RAM_CTRL: ARGB 1555, RGB 565, ARGB 4444, ARGB 8888)
void unpack(u32 v, u32 fmt, u8* p) {
	auto x4 = [](u32 v) { return (u8)(v * 17); };
	auto x5 = [](u32 v) { return (u8)((v << 3) | (v >> 2)); };
	auto x6 = [](u32 v) { return (u8)((v << 2) | (v >> 4)); };
	switch (fmt) {
		case 0: p[0] = x5((v >> 10) & 31), p[1] = x5((v >> 5) & 31), p[2] = x5(v & 31), p[3] = (v & 0x8000) ? 255 : 0; break;
		case 1: p[0] = x5((v >> 11) & 31), p[1] = x6((v >> 5) & 63), p[2] = x5(v & 31), p[3] = 255; break;
		case 2: p[0] = x4((v >> 8) & 15), p[1] = x4((v >> 4) & 15), p[2] = x4(v & 15), p[3] = x4((v >> 12) & 15); break;
		default: p[0] = (u8)(v >> 16), p[1] = (u8)(v >> 8), p[2] = (u8)v, p[3] = (u8)(v >> 24); break;
	}
}

}  // namespace

GdxsvFontRasterSource::GdxsvFontRasterSource() { gdxsv_custom_texture_cells_add_source(this, true); }

GdxsvFontRasterSource::~GdxsvFontRasterSource() { gdxsv_custom_texture_cells_remove_source(this); }

bool GdxsvFontRasterSource::loadMap() {
	message_chars_ready = false;
	if (!config::GdxFontRaster) return false;
	const auto start = std::chrono::steady_clock::now();
	draw_message_chars(font, message_chars);
	message_chars_ready = true;
	NOTICE_LOG(COMMON, "FontRaster: battle-message sheet's characters drawn in %d ms",
			   (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
	return true;
}

void GdxsvFontRasterSource::terminate() {
	message_chars_ready = false;
	message_chars.clear();
	{
		std::lock_guard<std::mutex> lock(message_vram_mutex);
		message_texels.clear();
		message_palette.clear();
	}
	font.Close();
}

void GdxsvFontRasterSource::OnLoadQueued(const BaseTextureCacheData& t) {
	if (!config::GdxFontRaster || t.texture_hash != kMessageSheetHash) return;
	// 8-bit palette indices, twiddled, and the 256 colours they select
	const size_t bytes = (size_t)kSheetTexels * kSheetTexels;
	if (t.width != kSheetTexels || t.height != kSheetTexels || t.tcw.PixelFmt != PixelPal8 || t.tcw.VQ_Comp || t.size < bytes) {
		WARN_LOG(COMMON, "FontRaster: battle-message sheet in an unexpected format (tcw %08x, %dx%d)", t.tcw.full, t.width, t.height);
		return;
	}
	std::lock_guard<std::mutex> lock(message_vram_mutex);
	message_texels.assign(&vram[t.mmStartAddress], &vram[t.mmStartAddress] + bytes);
	const u32 first = (t.tcw.PalSelect >> 4) << 8;
	message_palette.resize(256 * 4);
	for (int i = 0; i < 256; i++) unpack(PALETTE_RAM[first + i], PAL_RAM_CTRL & 3, &message_palette[i * 4]);
}

u8* GdxsvFontRasterSource::loadCustomTexture(u32 hash, int& width, int& height) {
	if (hash != kMessageSheetHash) return font.Render(hash, GdxsvFontStyle::Text, width, height);
	if (!message_chars_ready) return nullptr;
	u8* out = (u8*)std::malloc((size_t)kOut * kOut * 4);
	if (out == nullptr) return nullptr;
	int bw, bh;
	if (u8* base = gdxsv_custom_texture_cells_load_below(this, hash, bw, bh)) {
		// The sheet of a source below (the embedded textures, the texture pack) scaled to the output size
		for (int y = 0; y < kOut; y++)
			for (int x = 0; x < kOut; x++)
				memcpy(&out[((size_t)y * kOut + x) * 4], &base[((size_t)(y * bh / kOut) * bw + x * bw / kOut) * 4], 4);
		std::free(base);
	} else {
		// Else the game's texels scaled up, in the texture's row order like the other sources' images (their files are
		// flipped on load)
		std::lock_guard<std::mutex> lock(message_vram_mutex);
		if (message_texels.empty()) {
			std::free(out);
			return nullptr;
		}
		for (int y = 0; y < kSheetTexels; y++)
			for (int x = 0; x < kSheetTexels; x++) {
				const u8* p = &message_palette[message_texels[twiddled(x, y, kSheetTexels, kSheetTexels)] * 4];
				for (int j = 0; j < kOutScale; j++)
					for (int i = 0; i < kOutScale; i++)
						memcpy(&out[((size_t)(y * kOutScale + j) * kOut + x * kOutScale + i) * 4], p, 4);
			}
	}
	// The characters over it
	const int cell = kSheetCell * kOutScale;
	for (int r = 0; r < kSheetRowCount; r++)
		for (int c = 0; kSheetRows[r][c] != 0; c++) {
			if (!is_char_cell(r, c)) continue;
			for (int y = 0; y < cell; y++) {
				const size_t at = ((size_t)(kOut - 1 - (r * cell + y)) * kOut + c * cell) * 4;
				memcpy(&out[at], &message_chars[at], cell * 4);
			}
		}
	width = height = kOut;
	return out;
}

bool GdxsvFontRasterSource::isTextureReplaced(u32 hash) {
	return font.HasGlyph(hash) || (hash == kMessageSheetHash && message_chars_ready);
}

u8* GdxsvFontRasterSource::LoadCellTexture(u32 hash, int& width, int& height) {
	return shouldReplace() ? font.Render(hash, GdxsvFontStyle::BattleName, width, height) : nullptr;
}
