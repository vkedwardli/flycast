#pragma once

#include <string>
#include <vector>

#include "types.h"

// Debug overlay for DC2: lists the weapon objects (projectiles) currently in the game's entity pool.
// Memory layout: inada-s/gdxsv-reveng dc2/entity_pool.md
class GdxsvProjectileView {
   public:
	struct Entry {
		int slot;
		u8 cls;
		u8 kind;
		u8 type;
		int owner;  // player 0..3, -1 if unknown
		int owner_ms;
		float pos[3];
	};

	void DisplayOSD();

	// Reads the pool. Returns false outside a battle.
	static bool Collect(std::vector<Entry>& out, bool all_classes);
	static const char* KindName(u8 kind);
	static const char* MsName(int ms_id);

   private:
	std::vector<Entry> entries_;
};
