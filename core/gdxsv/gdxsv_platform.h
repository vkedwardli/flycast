// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

// Host services gdxsv needs from the operating system. One implementation per
// platform (gdxsv_platform_<platform>); an empty string where not available.

void gdxsv_open_url(const std::string& url);

// A stable id of this machine, or "".
std::string gdxsv_machine_id();

// "Wired", "Wireless", "VPN" / "VPN: <adapter>", "Unknown", or "".
std::string gdxsv_connection_medium();
