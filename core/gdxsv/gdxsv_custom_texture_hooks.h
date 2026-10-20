// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "types.h"

class BaseTextureCacheData;

// Called by the custom texture loader (core/rend/CustomTexture.cpp).

// On while the gdxsv texture pack source exists (DC2 only): the game draws text as textures made of 32x32 glyph cells.
extern bool gdxsv_custom_texture_cells_on;

void gdxsv_custom_texture_queue_cells(const BaseTextureCacheData* texture);
u8* gdxsv_custom_texture_build_cells(const BaseTextureCacheData* texture, int& width, int& height);

// Render thread, when the load is queued (after custom_load_in_progress++): keeps a copy of the texture's cells from VRAM
// if it is made of cells.
inline void gdxsv_custom_texture_on_queue(const BaseTextureCacheData* texture) {
	if (gdxsv_custom_texture_cells_on) gdxsv_custom_texture_queue_cells(texture);
}

// Loader thread, when the texture has no replacement of its own: the texture built from the kept cells' replacements, cells
// without one scaled up. Null if none was kept. The caller frees the RGBA image.
inline u8* gdxsv_custom_texture_load_cells(const BaseTextureCacheData* texture, int& width, int& height) {
	return gdxsv_custom_texture_cells_on ? gdxsv_custom_texture_build_cells(texture, width, height) : nullptr;
}
