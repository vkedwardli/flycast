// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "types.h"

class BaseTextureCacheData;

// A gdxsv texture source: FontRaster draws the cells; the sources below it supply the textures it draws over.
class GdxsvCellTextureSource {
   public:
	virtual ~GdxsvCellTextureSource() = default;
	// A texture's load is queued: its texels are in VRAM, and the source may keep a copy to build the replacement from.
	// Render thread.
	virtual void OnLoadQueued(const BaseTextureCacheData& texture) {}
	// The source's own replacement of the whole texture with this hash, as its loadCustomTexture returns it, or nullptr
	// if it has none or is off. Loader thread.
	virtual u8* LoadTexture(u32 hash, int& width, int& height) { return nullptr; }
	// The replacement of the 32x32 glyph with this hash as drawn in a cell texture: the in-battle pilot names, which
	// may be styled apart from other text. A malloc'ed RGBA image like loadCustomTexture's, or nullptr. Loader thread.
	virtual u8* LoadCellTexture(u32 hash, int& width, int& height) { return nullptr; }
};

// The gdxsv texture sources register while they exist; the cells' replacements are looked up in them, in this order.
// The FontRaster source turns the cell textures on.
void gdxsv_custom_texture_cells_add_source(GdxsvCellTextureSource* source, bool turn_on);
void gdxsv_custom_texture_cells_remove_source(GdxsvCellTextureSource* source);
// The first replacement of the texture with this hash among the sources registered after `source`, which it outranks.
// Loader thread.
u8* gdxsv_custom_texture_cells_load_below(const GdxsvCellTextureSource* source, u32 hash, int& width, int& height);
