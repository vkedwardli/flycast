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
// VRAM write protection shared by its users. The texture cache protects the pages of its textures to see them
// overwritten, memwatch protects VRAM to record the pages a rollback must restore, and the write tracker protects it
// to know the pages changed since the last frame given to the renderer. A page stays protected while one of them
// wants it, so that one user unprotecting a page does not hide the next write from the others.
#pragma once
#include "types.h"

#include <vector>

namespace vramprotect
{

enum Owner : u8
{
	TexCache = 1,
	MemWatch = 2,
	Tracker = 4,
};

// addr and size are in bytes and rounded to whole pages.
void protect(u32 addr, u32 size, u8 owners);
void unprotect(u32 addr, u32 size, u8 owners);
// A write faulted on this page: every owner has seen it, the page is unprotected.
void writeFault(u32 addr);
// Drops every owner and unprotects the whole VRAM (the VRAM size may change next).
void reset();

} // namespace vramprotect

// The VRAM pages written since the last take(). Write faults are seen on the emulation thread only.
namespace vramtracker
{

bool active();
// A write fault on this VRAM offset. The caller unprotects the page.
void written(u32 offset);
// Starts tracking on the first call, with every page written. Fills pages with the offsets of the pages written since
// the previous call and protects them again.
void take(std::vector<u32>& pages);
void stop();

// Test only (GDXSV_VRAM_TRACK_CHECK): at each call, compares the whole VRAM with its content at the previous call and
// counts the changed pages the tracker did not report. With render snapshots, rendsnap::capture checks its copy instead.
bool checkEnabled();
void check();

} // namespace vramtracker
