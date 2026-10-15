/*
	Copyright 2020 flyinghead

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
#ifndef FEAT_NO_MINIUPNPC
#include <cstddef>
#include <miniupnpc.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <mutex>

class MiniUPnP
{
public:
	MiniUPnP() {
		lanAddress[0] = 0;
		memset(&urls, 0, sizeof(urls));
		memset(&data, 0, sizeof(data));
		lastError[0] = 0; // gdxsv: error text for the P2P status
	}
	bool Init();
	void Term();
	bool AddPortMapping(int port, bool tcp);
	// gdxsv: port-mapping check, addresses and last error for the P2P status
	bool CheckPortMapping(int port, bool tcp);
	const char *localAddress() const { return lanAddress; }
	const char *externalAddress() const { return ""; } // not queried from the router
	const char *getLastError() const { return lastError; }
	bool isInitialized() const { return initialized; }

private:
	UPNPUrls urls;
	IGDdatas data;
	char lanAddress[32];
	char lastError[256]; // gdxsv
	std::vector<std::pair<std::string, bool>> mappedPorts;
	bool initialized = false;
	std::mutex mutex;
};

#else

class MiniUPnP
{
public:
	bool Init() { return true; }
	void Term() {}
	bool AddPortMapping(int port, bool tcp) { return true; }
	// gdxsv: P2P status
	bool CheckPortMapping(int port, bool tcp) { return false; }
	const char *localAddress() const { return ""; }
	const char *externalAddress() const { return ""; }
	const char *getLastError() const { return ""; }
	bool isInitialized() const { return false; }
};

#endif
