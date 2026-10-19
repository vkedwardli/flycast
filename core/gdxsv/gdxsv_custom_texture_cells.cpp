// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// DC2 draws some text as one texture made of 32x32 glyph cells (the battle pilot name sheet).
// A cell hashes like a 32x32 texture of the same format, so the glyph replacements also apply to the cells.

#include <xxhash.h>

#include <cstdlib>
#include <cstring>
#include <vector>

#include "gdxsv_custom_texture_hooks.h"
#include "hw/pvr/pvr_mem.h"
#include "rend/TexCache.h"

bool gdxsv_custom_texture_cells_on = false;

struct GdxsvTextureCells {
	int cols = 0;
	int rows = 0;
	std::vector<u32> hashes;  // per cell, row-major
	std::vector<u16> texels;  // whole texture, ARGB1555
};

namespace {

constexpr int kCell = 32;

bool is_cell_texture(const BaseTextureCacheData* t) {
	return t->tcw.PixelFmt == Pixel1555 && t->tcw.ScanOrder && !t->tcw.StrideSel && !t->tcw.VQ_Comp && t->width % kCell == 0 &&
		   t->height % kCell == 0 && t->width * t->height > kCell * kCell && t->size >= (u32)t->width * t->height * 2;
}

u8 expand5(u16 v) { return (u8)((v << 3) | (v >> 2)); }

}  // namespace

std::shared_ptr<const GdxsvTextureCells> gdxsv_custom_texture_read_cells(const BaseTextureCacheData* texture) {
	if (!is_cell_texture(texture)) return nullptr;
	const BaseTextureCacheData& t = *texture;
	auto cells = std::make_shared<GdxsvTextureCells>();
	cells->cols = t.width / kCell;
	cells->rows = t.height / kCell;
	cells->texels.resize(t.width * t.height);
	memcpy(cells->texels.data(), &vram[t.mmStartAddress], cells->texels.size() * 2);
	u16 cell[kCell * kCell];
	for (int r = 0; r < cells->rows; r++)
		for (int c = 0; c < cells->cols; c++) {
			for (int y = 0; y < kCell; y++) memcpy(&cell[y * kCell], &cells->texels[(r * kCell + y) * t.width + c * kCell], kCell * 2);
			// Same as BaseTextureCacheData::ComputeHash for a 32x32 texture with this tcw
			cells->hashes.push_back(XXH32(cell, sizeof(cell), 7) ^ (t.tcw.full & 0xFC000000));
		}
	return cells;
}

u8* gdxsv_custom_texture_load_cells(const std::shared_ptr<const GdxsvTextureCells>& grid,
									const std::function<u8*(u32 hash, int& width, int& height)>& load, int& width, int& height) {
	if (!grid) return nullptr;
	const GdxsvTextureCells& cells = *grid;
	std::vector<u8*> images(cells.hashes.size());
	int scale = 0;
	for (size_t i = 0; i < images.size(); i++) {
		int w, h;
		u8* data = load(cells.hashes[i], w, h);
		if (data == nullptr) continue;
		if (w != h || w % kCell != 0 || (scale != 0 && w != scale * kCell)) {
			free(data);
			continue;
		}
		scale = w / kCell;
		images[i] = data;
	}
	if (scale == 0) return nullptr;

	const int texW = cells.cols * kCell;
	const int cellW = kCell * scale;
	width = texW * scale;
	height = cells.rows * kCell * scale;
	u8* out = (u8*)malloc((size_t)width * height * 4);
	if (out != nullptr) {
		for (int r = 0; r < cells.rows; r++)
			for (int c = 0; c < cells.cols; c++) {
				const u8* img = images[r * cells.cols + c];
				for (int y = 0; y < cellW; y++) {
					u8* dst = out + ((size_t)(r * cellW + y) * width + c * cellW) * 4;
					if (img != nullptr) {
						memcpy(dst, img + (size_t)y * cellW * 4, cellW * 4);
						continue;
					}
					// No replacement: scale the original texels up
					const u16* src = &cells.texels[(r * kCell + y / scale) * texW + c * kCell];
					for (int x = 0; x < cellW; x++) {
						const u16 v = src[x / scale];
						dst[x * 4 + 0] = expand5((v >> 10) & 0x1f);
						dst[x * 4 + 1] = expand5((v >> 5) & 0x1f);
						dst[x * 4 + 2] = expand5(v & 0x1f);
						dst[x * 4 + 3] = (v & 0x8000) ? 255 : 0;
					}
				}
			}
	}
	for (u8* img : images) free(img);
	return out;
}
