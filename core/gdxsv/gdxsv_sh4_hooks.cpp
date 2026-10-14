// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_sh4_hooks.h"

#include "gdxsv_config.h"

void gdxsv_sh4_block_cycles(u32 addr, u32& guest_cycles) {
	if (!config::GdxSlowIdleLoopHack) return;
	if (settings.gdxsv.disk == 1 && addr == 0x0c2317fa) {
		guest_cycles = 1448;
	}
	if (settings.gdxsv.disk == 2 && addr == 0x0c2ccdba) {
		guest_cycles = 1448;
	}
}
