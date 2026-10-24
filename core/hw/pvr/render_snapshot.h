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
// Threaded rendering: the emulator state a frame is rendered from (VRAM, palette, fog table and the registers the
// texture cache reads) is copied when the frame is queued. The render thread reads only that copy and never touches
// VRAM protection, so the emulation thread does not wait for it, not even to load a rollback state.
// Two copies: the emulation thread fills the one the render thread is not using. Only the pages written since a copy
// was last filled are copied again (vramtracker).
// The render thread does not write VRAM either: render to texture is not copied back and the framebuffer is not
// emulated. To add them back, the emulation thread would wait for the frame at its RENDER_DONE and copy the result
// into VRAM itself, so that memwatch and vramtracker see the writes.
#pragma once
#include "types.h"

namespace rendsnap
{

// Snapshots are used with threaded rendering
bool enabled();

// Emulation thread, when a frame is queued: fills copy `index`, which the render thread is not using.
// replacing: copy `index` holds a frame that was queued and never rendered; the new frame takes over its changes.
void capture(int index, bool replacing);
// Render thread, before it processes the frame in copy `index`: invalidates the textures of the pages changed since
// the frame it rendered before, updates the palette and the fog table and points texSource to the copy.
void apply(int index);
// The next capture copies everything (emulation stopped).
void reset();

} // namespace rendsnap
