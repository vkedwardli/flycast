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
// Replay decides locally. Rollback: only peer 0 decides (LocalSlow) and sends the result as a flag
// in its GGPO input; every peer stalls on that synced flag (gdxsv_backend_rollback.cpp).
class GdxsvSlowdown {
   public:
	enum Category { Missile, Gun, Bazooka, Blast, Cracker, Beam, HeatRod, MsExplosion, HitSpark, NeedleMissile, Cannon, NumCategories };

	struct Load {
		float value = 0;
		std::array<int, NumCategories> counts{};
	};

	static bool InBattle();
	// Replay: called every vblank. Decides the stall and keeps the OSD numbers.
	void OnVBlank();
	// Replay: true while the current vblank must not deliver input.
	bool Stalling() const { return stalling_; }
	// This peer's own view: Slowdown enabled and the load at or over the threshold.
	bool LocalSlow();
	// Rollback: the synced slowdown state of the current frame, for the OSD.
	void SetSynced(bool slow) { synced_slow_ = slow; }
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
	bool synced_slow_ = false;
	Load load_;
	std::vector<GdxsvProjectileView::Entry> entries_;
	std::vector<GdxsvProjectileView::Entry> osd_entries_;

	// Per battle, for tuning the threshold: the arcade spends ~11% of a battle at 30fps, ~1s per slowdown.
	int battle_vblanks_ = 0;
	int slow_vblanks_ = 0;
	int slow_runs_ = 0;
	bool slow_ = false;
};
