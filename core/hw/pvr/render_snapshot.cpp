/*
	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
// gdxsv: added by the fork (docs/gdxsv/upstream.md)
#include "render_snapshot.h"
#include "pvr_mem.h"
#include "pvr_regs.h"
#include "Renderer_if.h"
#include "cfg/option.h"
#include "hw/mem/vram_protect.h"
#include "rend/TexCache.h"
#include "rend/texconv.h"
#include "stdclass.h"

#include <array>
#include <cstring>
#include <memory>
#include <vector>

namespace rendsnap
{

constexpr u32 MaxPages = VRAM_SIZE_MAX / PAGE_SIZE;
constexpr u32 PaletteSize = 1024;
constexpr u32 FogTableSize = 128;

struct Copy
{
	std::unique_ptr<u8[]> vram;
	u32 vramSize = 0;
	std::array<bool, MaxPages> stale;	// pages written since this copy was filled
	// For the render thread: what changed since the frame captured before this one
	std::vector<u32> changedPages;
	bool paletteChanged = false;
	bool fogChanged = false;

	u32 palette[PaletteSize];
	u32 fogTable[FogTableSize];
	u32 palRamCtrl = 0;
	u32 textControl = 0;
};

static Copy copies[2];
static int latest = -1;	// the copy filled last; -1: none since the reset
static std::vector<u32> writtenPages;

bool enabled() {
	return config::ThreadedRendering;
}

void capture(int index, bool replacing)
{
	Copy& copy = copies[index];
	if (copy.vramSize != VRAM_SIZE)
	{
		copy.vram = std::make_unique<u8[]>(VRAM_SIZE);
		copy.vramSize = VRAM_SIZE;
		copy.stale.fill(true);
	}
	if (!replacing)
	{
		copy.changedPages.clear();
		copy.paletteChanged = false;
		copy.fogChanged = false;
	}
	// The latest copy has the state of the previous frame: what differs from it changed since.
	const Copy *prev = latest == -1 || copies[latest].vramSize != VRAM_SIZE ? nullptr : &copies[latest];

	vramtracker::take(writtenPages);
	for (u32 offset : writtenPages)
	{
		const u32 page = offset / PAGE_SIZE;
		copies[0].stale[page] = true;
		copies[1].stale[page] = true;
		if (prev == nullptr || memcmp(&prev->vram[offset], &vram[offset], PAGE_SIZE) != 0)
			copy.changedPages.push_back(offset);
	}
	for (u32 page = 0; page < VRAM_SIZE / PAGE_SIZE; page++)
	{
		if (!copy.stale[page])
			continue;
		copy.stale[page] = false;
		const u32 offset = page * PAGE_SIZE;
		if (memcmp(&copy.vram[offset], &vram[offset], PAGE_SIZE) != 0)
			memcpy(&copy.vram[offset], &vram[offset], PAGE_SIZE);
	}

	if (prev == nullptr || prev->palRamCtrl != PAL_RAM_CTRL || memcmp(prev->palette, PALETTE_RAM, sizeof(copy.palette)) != 0)
		copy.paletteChanged = true;
	if (prev == nullptr || memcmp(prev->fogTable, FOG_TABLE, sizeof(copy.fogTable)) != 0)
		copy.fogChanged = true;
	memcpy(copy.palette, PALETTE_RAM, sizeof(copy.palette));
	memcpy(copy.fogTable, FOG_TABLE, sizeof(copy.fogTable));
	copy.palRamCtrl = PAL_RAM_CTRL;
	copy.textControl = TEXT_CONTROL;
	latest = index;

	if (vramtracker::checkEnabled())
	{
		// Test only: the copy must be the VRAM now
		static u64 captures, mismatches;
		captures++;
		for (u32 offset = 0; offset < VRAM_SIZE; offset += PAGE_SIZE)
			if (memcmp(&copy.vram[offset], &vram[offset], PAGE_SIZE) != 0)
			{
				if (mismatches < 20)
					ERROR_LOG(PVR, "RENDSNAP copy %d page %x differs at capture %llu", index, offset, (unsigned long long)captures);
				mismatches++;
			}
		if (captures % 3600 == 0)
			NOTICE_LOG(PVR, "RENDSNAP captures=%llu mismatches=%llu", (unsigned long long)captures, (unsigned long long)mismatches);
	}
}

void apply(int index)
{
	Copy& copy = copies[index];
	for (u32 offset : copy.changedPages)
		VramInvalidatePage(offset);
	copy.changedPages.clear();
	if (copy.paletteChanged)
	{
		palette_update(copy.palette, copy.palRamCtrl);
		copy.paletteChanged = false;
	}
	if (copy.fogChanged)
	{
		rend_updateFogTable();
		copy.fogChanged = false;
	}
	texSource = { copy.vram.get(), copy.textControl, copy.palRamCtrl, copy.fogTable };
}

void reset()
{
	latest = -1;
	for (Copy& copy : copies)
		copy.stale.fill(true);
}

} // namespace rendsnap
