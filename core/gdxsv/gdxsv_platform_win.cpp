// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_platform.h"

// clang-format off: winsock2.h must come before windows.h and the IP helper headers
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <windows.h>
#include <shellapi.h>
// clang-format on

#include <cstring>
#include <cwctype>
#include <nowide/stackstring.hpp>

void gdxsv_open_url(const std::string& url) { ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOW); }

std::string gdxsv_machine_id() {
	char value[255];
	DWORD BufferSize = sizeof(value);
	LONG res =
		RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid", RRF_RT_REG_SZ, NULL, value, &BufferSize);
	if (res == 0) {
		std::string s;
		s += value;
		return s;
	} else {
		return "";
	}
}

std::string gdxsv_connection_medium() {
	std::string result = "Unknown";

	DWORD dwIfIndex4 = 0;
	GetBestInterface(inet_addr("8.8.8.8"), &dwIfIndex4);

	DWORD dwIfIndex6 = 0;
	sockaddr_in6 addr6{};
	addr6.sin6_family = AF_INET6;
	// Google DNS IPv6: 2001:4860:4860::8888
	const unsigned char google_dns_ipv6[] = {0x20, 0x01, 0x48, 0x60, 0x48, 0x60, 0x00, 0x00,
											 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x88, 0x88};
	memcpy(&addr6.sin6_addr, google_dns_ipv6, 16);
	GetBestInterfaceEx((sockaddr*)&addr6, &dwIfIndex6);

	auto check_vpn = [&](DWORD dwIfIndex) -> bool {
		if (dwIfIndex == 0) return false;
		MIB_IF_ROW2 row{};
		row.InterfaceIndex = dwIfIndex;
		if (NO_ERROR == GetIfEntry2(&row)) {
			auto has_vpn_keyword = [](const wchar_t* s) {
				if (!s) return false;
				std::wstring u(s);
				for (auto& c : u) c = (wchar_t)std::towupper(c);
				return u.find(L"VPN") != std::wstring::npos || u.find(L"WIREGUARD") != std::wstring::npos ||
					   u.find(L"WINTUN") != std::wstring::npos || u.find(L"TAP-") != std::wstring::npos ||
					   u.find(L"WAN MINIPORT") != std::wstring::npos || u.find(L"TUNNEL") != std::wstring::npos;
			};

			if (row.Type == IF_TYPE_TUNNEL || row.Type == IF_TYPE_PPP || row.Type == IF_TYPE_PROP_VIRTUAL ||
				has_vpn_keyword(row.Description) || has_vpn_keyword(row.Alias)) {
				result = "VPN";
				nowide::stackstring name;
				if (row.Alias[0] && has_vpn_keyword(row.Alias))
					name.convert(row.Alias);
				else
					name.convert(row.Description);
				if (name.get() && name.get()[0]) result = "VPN: " + std::string(name.get());
				return true;
			}
		}
		return false;
	};
	if (check_vpn(dwIfIndex6) || check_vpn(dwIfIndex4)) return result;

	DWORD dwIfIndex = dwIfIndex6 != 0 ? dwIfIndex6 : dwIfIndex4;
	if (dwIfIndex == 0) return result;
	MIB_IF_ROW2 row{};
	row.InterfaceIndex = dwIfIndex;

	if (NOERROR == GetIfEntry2(&row)) {
		switch (row.PhysicalMediumType) {
			case NdisPhysicalMediumWirelessLan:
			case NdisPhysicalMediumWirelessWan:
			case NdisPhysicalMediumNative802_11:
			case NdisPhysicalMediumBluetooth:
			case NdisPhysicalMediumWiMax:
			case NdisPhysicalMediumUWB:
			case NdisPhysicalMediumIrda:
				result = "Wireless";
				break;
			case NdisPhysicalMediumCableModem:
			case NdisPhysicalMediumPhoneLine:
			case NdisPhysicalMediumPowerLine:
			case NdisPhysicalMediumDSL:
			case NdisPhysicalMediumFibreChannel:
			case NdisPhysicalMedium1394:
			case NdisPhysicalMediumInfiniband:
			case NdisPhysicalMedium802_3:
			case NdisPhysicalMedium802_5:
			case NdisPhysicalMediumWiredWAN:
			case NdisPhysicalMediumWiredCoWan:
				result = "Wired";
				break;
			default:
				// leave as "Unknown"
				break;
		}
	}
	return result;
}
