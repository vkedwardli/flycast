// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "types.h"

// Called by the SH4 block decoder after it sets a block's cycle count.
// Slow idle loop hack: the game's idle loop block costs more cycles.
void gdxsv_sh4_block_cycles(u32 addr, u32& guest_cycles);
