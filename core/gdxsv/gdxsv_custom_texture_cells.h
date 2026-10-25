// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "types.h"

// A gdxsv texture source the cells' replacements are looked up in.
class GdxsvCellTextureSource {
   public:
	virtual ~GdxsvCellTextureSource() = default;
	// The replacement of the 32x32 glyph with this hash as drawn in a cell texture: the in-battle pilot names, which
	// may be styled apart from other text. A malloc'ed RGBA image like loadCustomTexture's, or nullptr. Loader thread.
	virtual u8* LoadCellTexture(u32 hash, int& width, int& height) = 0;
};

// The gdxsv texture sources register while they exist; the cells' replacements are looked up in them, in this order.
// The texture pack source turns the cell textures on.
void gdxsv_custom_texture_cells_add_source(GdxsvCellTextureSource* source, bool turn_on);
void gdxsv_custom_texture_cells_remove_source(GdxsvCellTextureSource* source);
