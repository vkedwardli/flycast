// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

class BaseCustomTextureSource;

// The gdxsv texture sources register while they exist; the cells' replacements are looked up in them, in this order.
// The texture pack source turns the cell textures on.
void gdxsv_custom_texture_cells_add_source(BaseCustomTextureSource* source, bool turn_on);
void gdxsv_custom_texture_cells_remove_source(BaseCustomTextureSource* source);
