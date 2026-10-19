// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <functional>
#include <memory>

#include "types.h"

class BaseTextureCacheData;
struct GdxsvTextureCells;

// Called by the custom texture loader (core/rend/CustomTexture.cpp).

// Set by CustomTexture::init: the game draws text as textures made of 32x32 glyph cells (DC2).
extern bool gdxsv_custom_texture_cells_on;

// When the load is queued: the texture's cells and their hashes, read from VRAM; null if it is not made of cells.
std::shared_ptr<const GdxsvTextureCells> gdxsv_custom_texture_read_cells(const BaseTextureCacheData* texture);

inline std::shared_ptr<const GdxsvTextureCells> gdxsv_custom_texture_cells(const BaseTextureCacheData* texture) {
	return gdxsv_custom_texture_cells_on ? gdxsv_custom_texture_read_cells(texture) : nullptr;
}

// On the loader thread, when the texture has no replacement of its own: the texture built from the cells' replacements
// (load(hash) per cell), cells without one scaled up. Null if no cell is replaced. The caller frees the RGBA image.
u8* gdxsv_custom_texture_load_cells(const std::shared_ptr<const GdxsvTextureCells>& cells,
									const std::function<u8*(u32 hash, int& width, int& height)>& load, int& width, int& height);
