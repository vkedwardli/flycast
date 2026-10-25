// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// DC2 draws some text as one texture made of 32x32 glyph cells (the battle pilot name sheet).
// A cell hashes like a 32x32 texture of the same format, so the glyph replacements also apply to the cells; the sources
// may draw them in another style.

#include "gdxsv_custom_texture_cells.h"

#include <xxhash.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "gdxsv_custom_texture_hooks.h"
#include "hw/pvr/pvr_mem.h"
#include "rend/TexCache.h"

bool gdxsv_custom_texture_cells_on = false;

namespace {

constexpr int kCell = 32;

// A texture's texels, copied from VRAM when its load was queued.
struct GdxsvTextureCells {
	int cols = 0;
	int rows = 0;
	// tcw bits mixed into the texture hash
	u32 tcw_hash = 0;
	// whole texture, ARGB1555
	std::vector<u16> texels;
};

struct GdxsvTextureCellsState {
	// Set and cleared on the render thread while no load runs (CustomTexture::init / terminate).
	std::vector<GdxsvCellTextureSource*> sources;

	// The latest copy per texture, until its last queued load has run or the next load is queued (a load that found the
	// texture's own replacement leaves it). A texture is not deleted while a load is queued.
	std::mutex kept_mutex;
	std::unordered_map<const BaseTextureCacheData*, std::shared_ptr<const GdxsvTextureCells>> kept;
} state;

bool is_cell_texture(const BaseTextureCacheData* t) {
	return t->tcw.PixelFmt == Pixel1555 && t->tcw.ScanOrder && !t->tcw.StrideSel && !t->tcw.VQ_Comp && t->width % kCell == 0 &&
		   t->height % kCell == 0 && t->width * t->height > kCell * kCell && t->size >= (u32)t->width * t->height * 2;
}

u8* load_replacement(u32 hash, int& width, int& height) {
	for (GdxsvCellTextureSource* source : state.sources)
		if (u8* data = source->LoadCellTexture(hash, width, height)) return data;
	return nullptr;
}

u8 expand5(u16 v) { return (u8)((v << 3) | (v >> 2)); }

u8* build(const GdxsvTextureCells& cells, int& width, int& height) {
	const int texW = cells.cols * kCell;
	std::vector<u8*> images(cells.cols * cells.rows);
	int scale = 0;
	u16 cell[kCell * kCell];
	for (int r = 0; r < cells.rows; r++)
		for (int c = 0; c < cells.cols; c++) {
			for (int y = 0; y < kCell; y++) memcpy(&cell[y * kCell], &cells.texels[(r * kCell + y) * texW + c * kCell], kCell * 2);
			// Same as BaseTextureCacheData::ComputeHash for a 32x32 texture with this tcw
			const u32 hash = XXH32(cell, sizeof(cell), 7) ^ cells.tcw_hash;
			int w, h;
			u8* data = load_replacement(hash, w, h);
			if (data == nullptr) continue;
			if (w != h || w % kCell != 0 || (scale != 0 && w != scale * kCell)) {
				free(data);
				continue;
			}
			scale = w / kCell;
			images[r * cells.cols + c] = data;
		}
	if (scale == 0) return nullptr;

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

}  // namespace

void gdxsv_custom_texture_cells_add_source(GdxsvCellTextureSource* source, bool turn_on) {
	state.sources.push_back(source);
	if (turn_on) gdxsv_custom_texture_cells_on = true;
}

void gdxsv_custom_texture_cells_remove_source(GdxsvCellTextureSource* source) {
	state.sources.erase(std::remove(state.sources.begin(), state.sources.end(), source), state.sources.end());
	if (state.sources.empty()) {
		// terminate drops queued loads: their copies are never taken
		gdxsv_custom_texture_cells_on = false;
		std::lock_guard<std::mutex> lock(state.kept_mutex);
		state.kept.clear();
	}
}

u8* gdxsv_custom_texture_cells_load_below(const GdxsvCellTextureSource* source, u32 hash, int& width, int& height) {
	auto it = std::find(state.sources.begin(), state.sources.end(), source);
	if (it == state.sources.end()) return nullptr;
	for (++it; it != state.sources.end(); ++it)
		if (u8* data = (*it)->LoadTexture(hash, width, height)) return data;
	return nullptr;
}

void gdxsv_custom_texture_queue_cells(const BaseTextureCacheData* texture) {
	for (GdxsvCellTextureSource* source : state.sources) source->OnLoadQueued(*texture);
	if (!is_cell_texture(texture)) {
		// A copy left by a deleted texture at the same address
		std::lock_guard<std::mutex> lock(state.kept_mutex);
		state.kept.erase(texture);
		return;
	}
	const BaseTextureCacheData& t = *texture;
	auto cells = std::make_shared<GdxsvTextureCells>();
	cells->cols = t.width / kCell;
	cells->rows = t.height / kCell;
	cells->tcw_hash = t.tcw.full & 0xFC000000;
	cells->texels.resize(t.width * t.height);
	memcpy(cells->texels.data(), &vram[t.mmStartAddress], cells->texels.size() * 2);
	std::lock_guard<std::mutex> lock(state.kept_mutex);
	state.kept[texture] = std::move(cells);
}

u8* gdxsv_custom_texture_build_cells(const BaseTextureCacheData* texture, int& width, int& height) {
	std::shared_ptr<const GdxsvTextureCells> cells;
	{
		std::lock_guard<std::mutex> lock(state.kept_mutex);
		const auto it = state.kept.find(texture);
		if (it == state.kept.end()) return nullptr;
		cells = it->second;
		// The count includes this load and is raised before a new copy is kept
		if (texture->custom_load_in_progress <= 1) state.kept.erase(it);
	}
	return build(*cells, width, height);
}
