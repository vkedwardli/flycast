/*
	Copyright 2021 flyinghead

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
#include <future>
#include <atomic> // gdxsv: timeSyncFrames
#include <string>

#include "types.h" // gdxsv: u16 in setExInput

struct MapleInputState;

namespace ggpo
{

std::future<bool> startNetwork();
void startSession(int localPort, int localPlayerNum);
void stopSession();
void getInput(MapleInputState inputState[4]);
bool nextFrame();
bool active();
void displayStats();
void endOfFrame();
bool getCurrentFrame(int* frame); // gdxsv
void sendChatMessage(int playerNum, const std::string& msg);
void receiveChatMessages(void (*callback)(int playerNum, const std::string& msg));
bool poll(); // gdxsv

static inline bool rollbacking() {
	extern bool inRollback;

	return inRollback;
}

// gdxsv: skip frames and extra input of the gdxsv session
void notifySkipInput();
// A replayed timesync skip held back the input the game asked for (advance_frame).
void notifySkipReplayHeld();
int getSkippedFrames(int frame = -1);

static inline void setExInput(u16 exInput) {
	extern u16 localExInput;
	localExInput = exInput;
}

// Flag bits sent ORed into exInput; setExInput leaves them alone.
static inline void setExInputFlags(u16 flags) {
	extern u16 localExInputFlags;
	localExInputFlags = flags;
}

extern std::atomic<int> timeSyncFrames;
}
