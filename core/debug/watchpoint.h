/*
	Copyright 2024 flyinghead

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
#pragma once
#include "types.h"
#include <vector>
#include <mutex>

namespace watchpoint
{

enum WatchType {
    WATCH_WRITE = 2,
    WATCH_READ = 3,
    WATCH_ACCESS = 4
};

struct Watchpoint {
    u32 addr;
    u32 len;
    WatchType type;
};

// Initialize watchpoint system
void init();

// Reset all watchpoints
void reset();

// Add a watchpoint
bool add(WatchType type, u32 addr, u32 len);

// Remove a watchpoint
bool remove(WatchType type, u32 addr, u32 len);

// Check if any read watchpoint matches the address range
// Returns true if a watchpoint was hit
bool checkRead(u32 addr, u32 len);

// Check if any write watchpoint matches the address range
// Returns true if a watchpoint was hit
bool checkWrite(u32 addr, u32 len);

// Get the last hit watchpoint address (for reporting to GDB)
u32 getLastHitAddr();

// Check if watchpoints are active (for fast path optimization)
bool hasActiveWatchpoints();

} // namespace watchpoint
