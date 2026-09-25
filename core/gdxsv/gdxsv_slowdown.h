#pragma once

#include <array>
#include <vector>

#include "gdxsv_projectile_view.h"
#include "types.h"

// Experimental: reproduce the arcade/DC slowdown in DC2 battles.
// Weigh the projectiles in play; at or over the threshold the battle simulation steps at 30fps
// (the game steps once per delivered KeyMsg1, so every other frame delivers no input).
// Weights: ac-slowdown-test ANALYSIS.md section 2 (fitted on 145 arcade matches, gun shot = 1).
//
// Rollback: the load is read only from emulated memory, so every peer, and every rollback pass,
// decides the same stall for the same frame.
// All peers must use the same Slowdown options.
class GdxsvSlowdown {
   public:
	enum Category { Missile, Gun, Bazooka, BazookaBlast, Cracker, CrackerBlast, Beam, HeatRod, NumCategories };

	struct Load {
		float value = 0;
		std::array<int, NumCategories> counts{};
	};

	// Replay: called every vblank. Decides the stall and keeps the OSD numbers.
	void OnVBlank();
	// Replay: true while the current vblank must not deliver input.
	bool Stalling() const { return stalling_; }
	// Rollback: true if GGPO frame `frame` must not deliver the battle input.
	// Call where the frame's input would be delivered.
	bool StallFrame(int frame);
	void DisplayOSD();

	static int Classify(const GdxsvProjectileView::Entry& e);
	static const char* CategoryName(int category);
	static float Weight(int category);

   private:
	// Reads the projectiles from memory. Returns false outside a DC2 battle.
	static bool Measure(std::vector<GdxsvProjectileView::Entry>& scratch, Load& out);
	void CountVBlank(bool slow);
	void EndBattle();

	bool in_battle_ = false;
	bool stalling_ = false;
	Load load_;
	std::vector<GdxsvProjectileView::Entry> entries_;

	// Per battle, for tuning the threshold: the arcade spends ~11% of a battle at 30fps, ~1s per slowdown.
	int battle_vblanks_ = 0;
	int slow_vblanks_ = 0;
	int slow_runs_ = 0;
	bool slow_ = false;
	int rollback_decisions_ = 0;
	int rollback_stalls_ = 0;
};
