// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>

#include "gdxsv_platform.h"

void gdxsv_open_url(const std::string& url) {
	auto cmd = "xdg-open " + url;
	auto rv = system(cmd.c_str());
	(void)rv;
}

std::string gdxsv_machine_id() { return ""; }

std::string gdxsv_connection_medium() { return ""; }
