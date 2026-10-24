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
#include "vram_protect.h"
#include "addrspace.h"
#include "hw/pvr/pvr_mem.h"
#include "stdclass.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace vramtracker
{
static void restart();
}

namespace vramprotect
{

constexpr u32 MaxPages = VRAM_SIZE_MAX / PAGE_SIZE;
static std::array<u8, MaxPages> pageOwners;
// The texture cache protects from the render thread, the others from the emulation thread.
static std::mutex mutex;

// Protects or unprotects the pages in [first, end) whose owners went from none to some or back, a run at a time.
static void apply(u32 first, u32 end, const std::array<bool, MaxPages>& wasProtected)
{
	u32 page = first;
	while (page < end)
	{
		const bool target = pageOwners[page] != 0;
		if (wasProtected[page] == target)
		{
			page++;
			continue;
		}
		u32 runEnd = page + 1;
		while (runEnd < end && wasProtected[runEnd] != target && (pageOwners[runEnd] != 0) == target)
			runEnd++;
		if (target)
			addrspace::protectVram(page * PAGE_SIZE, (runEnd - page) * PAGE_SIZE);
		else
			addrspace::unprotectVram(page * PAGE_SIZE, (runEnd - page) * PAGE_SIZE);
		page = runEnd;
	}
}

template<typename F>
static void update(u32 addr, u32 size, F f)
{
	if (VRAM_SIZE == 0 || addr >= VRAM_SIZE)
		return;
	size = std::min(VRAM_SIZE - addr, size);
	const u32 first = addr / PAGE_SIZE;
	const u32 end = (addr + size + PAGE_SIZE - 1) / PAGE_SIZE;
	static std::array<bool, MaxPages> wasProtected;
	std::lock_guard<std::mutex> _(mutex);
	for (u32 page = first; page < end; page++)
	{
		wasProtected[page] = pageOwners[page] != 0;
		f(pageOwners[page]);
	}
	apply(first, end, wasProtected);
}

void protect(u32 addr, u32 size, u8 owners)
{
	update(addr, size, [owners](u8& o) { o |= owners; });
}

void unprotect(u32 addr, u32 size, u8 owners)
{
	update(addr, size, [owners](u8& o) { o &= ~owners; });
}

void writeFault(u32 addr)
{
	if (VRAM_SIZE == 0 || addr >= VRAM_SIZE)
		return;
	std::lock_guard<std::mutex> _(mutex);
	pageOwners[addr / PAGE_SIZE] = 0;
	// Whatever the owners said: a page left protected would fault on this write forever.
	addrspace::unprotectVram(addr & ~PAGE_MASK, PAGE_SIZE);
}

void reset()
{
	{
		std::lock_guard<std::mutex> _(mutex);
		pageOwners.fill(0);
		if (VRAM_SIZE != 0)
			addrspace::unprotectVram(0, VRAM_SIZE);
	}
	vramtracker::restart();
}

} // namespace vramprotect

namespace vramtracker
{

constexpr u32 MaxPages = vramprotect::MaxPages;
static bool started;
static std::array<u64, (MaxPages + 63) / 64> writtenPages;

bool active() {
	return started;
}

void written(u32 offset)
{
	if (!started || offset >= VRAM_SIZE)
		return;
	const u32 page = offset / PAGE_SIZE;
	writtenPages[page / 64] |= u64{1} << (page % 64);
}

void take(std::vector<u32>& pages)
{
	pages.clear();
	if (VRAM_SIZE == 0)
		return;
	const u32 count = VRAM_SIZE / PAGE_SIZE;
	if (!started)
	{
		started = true;
		writtenPages.fill(0);
		for (u32 page = 0; page < count; page++)
			pages.push_back(page * PAGE_SIZE);
		vramprotect::protect(0, VRAM_SIZE, vramprotect::Tracker);
		return;
	}
	for (u32 w = 0; w < (count + 63) / 64; w++)
	{
		u64 bits = writtenPages[w];
		writtenPages[w] = 0;
		while (bits != 0)
		{
			const u32 page = w * 64 + __builtin_ctzll(bits);
			bits &= bits - 1;
			pages.push_back(page * PAGE_SIZE);
		}
	}
	// Runs of pages protected again together
	for (size_t i = 0; i < pages.size();)
	{
		size_t j = i + 1;
		while (j < pages.size() && pages[j] == pages[j - 1] + PAGE_SIZE)
			j++;
		vramprotect::protect(pages[i], (u32)(j - i) * PAGE_SIZE, vramprotect::Tracker);
		i = j;
	}
}

void stop()
{
	if (!started)
		return;
	started = false;
	vramprotect::unprotect(0, VRAM_SIZE, vramprotect::Tracker);
}

static void restart()
{
	started = false;
}

bool checkEnabled()
{
	static const bool enabled = getenv("GDXSV_VRAM_TRACK_CHECK") != nullptr;
	return enabled;
}

void check()
{
	static std::vector<u8> copy;
	static std::vector<u32> pages;
	static std::vector<bool> reported;
	static u64 calls, reportedPages, changedPages, missedPages;
	take(pages);
	if (copy.size() != VRAM_SIZE)
	{
		// first call: the tracker reports every page
		copy.assign(&vram[0], &vram[0] + VRAM_SIZE);
		return;
	}
	reported.assign(VRAM_SIZE / PAGE_SIZE, false);
	for (u32 offset : pages)
		reported[offset / PAGE_SIZE] = true;
	calls++;
	reportedPages += pages.size();
	for (u32 page = 0; page < VRAM_SIZE / PAGE_SIZE; page++)
	{
		u8 *mem = &vram[page * PAGE_SIZE];
		if (memcmp(&copy[page * PAGE_SIZE], mem, PAGE_SIZE) == 0)
			continue;
		changedPages++;
		if (!reported[page])
		{
			if (missedPages < 20)
				ERROR_LOG(PVR, "VRAMTRACK missed page %x at call %llu", page * PAGE_SIZE, (unsigned long long)calls);
			missedPages++;
		}
		memcpy(&copy[page * PAGE_SIZE], mem, PAGE_SIZE);
	}
	if (calls % 3600 == 0)
		NOTICE_LOG(PVR, "VRAMTRACK calls=%llu reported=%llu changed=%llu missed=%llu", (unsigned long long)calls,
				(unsigned long long)reportedPages, (unsigned long long)changedPages, (unsigned long long)missedPages);
}

} // namespace vramtracker
