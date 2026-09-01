#pragma once

#include <unordered_map>

#include "hw/mem/mem_watch.h"

class GdxsvSaveState {
   public:
	void StartUsing();
	void EndUsing();
	bool Enabled() { return enabled; }
	size_t SavedFrames() { return buffers.size(); }
	int LastSavedFrame() { return buffers.empty() ? -1 : buffers.rbegin()->first; }
	int FirstSavedFrame() { return buffers.empty() ? -1 : buffers.begin()->first; }
	int FindSavedFrameAtOrBefore(int frame) const;
	bool SaveState(int frame);
	bool LoadState(int frame);
	bool LoadStateMostRecent(int& frame);
	void Clear();
	void Reset();

   private:
	bool LoadStateInternal(int frame);
	struct MemPages {
		void load() {
			memwatch::ramWatcher.getPages(ram);
			memwatch::vramWatcher.getPages(vram);
			memwatch::aramWatcher.getPages(aram);
			memwatch::elanWatcher.getPages(elanram);
		}
		memwatch::PageMap ram;
		memwatch::PageMap vram;
		memwatch::PageMap aram;
		memwatch::PageMap elanram;
	};

	bool enabled = false;
	std::map<int, MemPages> deltaStates;
	std::map<int, std::pair<int, unsigned char*>> buffers;
};

extern GdxsvSaveState gdxsv_save_state;
