#include "gdxsv_slowdown.h"

#include <map>
#include <tuple>

#include "cfg/option.h"
#include "gdxsv.h"
#include "imgui/imgui.h"
#include "network/ggpo.h"

namespace {
constexpr u8 kWeaponClass = 0x11;
constexpr u8 kEffectClass = 0x13;
// MS destroyed: 22-29 class 0x13 kind 25 objects (the debris) appear 10-16 vblanks after its HP reaches 0
// and last ~1.5s (97 of 98 spawns in replay 1790270343703 were right after a kill). Counted per object,
// so the load fades as the debris goes and two explosions weigh double.
constexpr u8 kMsExplosionKind = 25;
// Hit spark: class 0x13 kind 10, no owner, ~2s. Came with 29/32 shot hits in the same replay;
// melee hits show it too, sometimes.
constexpr u8 kHitSparkKind = 10;
}  // namespace

// Weapon objects (class 0x11) by kind, measured with a pool + ammo probe on 5 replays.
// Explosions of bazooka shells and crackers are kind 6 objects of their own, so they get
// the blast weights of ANALYSIS.md 2.2.1 instead of a fixed lifetime after the shot.
int GdxsvSlowdown::Classify(const GdxsvProjectileView::Entry& e) {
	if (e.cls == kEffectClass && e.kind == kHitSparkKind) return HitSpark;
	if (e.cls == kEffectClass && e.kind == kMsExplosionKind) return MsExplosion;
	if (e.cls != kWeaponClass) return -1;
	switch (e.kind) {
		case 1:	 // beam rifles, Z'Gok arm beams
		case 4:	 // Dom chest beam
		case 8:	 // Gogg diffuse beam, 12 objects per shot
			return Beam;
		case 2:	 // vulcans, machine guns, Acguy and Z'Gok shots (the arcade fit counts Z'Gok missiles as gun)
			return Gun;
		case 28:  // Gogg torpedo, 2 per shot
			return Missile;
		case 26:  // Gyan needle missiles, types 00-09: 10 per shot (rbk_test probe)
			return NeedleMissile;
		case 14:  // Gouf heat rod: type 00 is the rod, held all battle; type 01 is the 8-object burst while it is used
			return e.type == 0x01 ? HeatRod : -1;
		case 3:	 // cannon shells (Guntank types 03/04, Guncannon, GT Gundam); no blast object
			return Cannon;
		case 5:	 // bazooka shells (Char's Zaku, Dom)
			return Bazooka;
		case 7:	  // Char's Zaku cracker
		case 29:  // Zaku cracker
			return Cracker;
		case 6:	 // blast: bazooka shell, cracker (type 0a), torpedo, Gyan sub
			return Blast;
		default:
			return -1;
	}
}

const char* GdxsvSlowdown::CategoryName(int category) {
	static const char* names[NumCategories] = {"Missile", "Gun", "Bazooka", "Blast", "Cracker", "Beam", "Heat rod", "MS explosion", "Hit spark", "Needle missile", "Cannon"};
	return 0 <= category && category < NumCategories ? names[category] : "?";
}

float GdxsvSlowdown::Weight(int category) {
	static const float weights[NumCategories] = {1.24f, 0.9f, 0.3f, 0.6f, 0.1f, 1.0f, 0.11f, 0.15f, 0.1f, 0.3f, 0.3f};
	return 0 <= category && category < NumCategories ? weights[category] : 0.f;
}

bool GdxsvSlowdown::Measure(std::vector<GdxsvProjectileView::Entry>& scratch, Load& out) {
	out = Load{};
	if (!GdxsvProjectileView::Collect(scratch, true)) return false;
	for (const auto& e : scratch) {
		const int c = Classify(e);
		if (0 <= c) out.counts[c]++;
	}
	for (int c = 0; c < NumCategories; c++) out.value += Weight(c) * out.counts[c];
	return true;
}

bool GdxsvSlowdown::InBattle() { return GdxsvProjectileView::InBattle(); }

void GdxsvSlowdown::OnVBlank() {
	// Rollback: the stall is decided in gdxsv_backend_rollback.cpp; count what was synced.
	if (ggpo::active()) {
		if (ggpo::isInRollback()) return;
		if (!InBattle()) {
			if (in_battle_) EndBattle();
			return;
		}
		in_battle_ = true;
		if (config::GdxProjectileView) Measure(entries_, load_);  // the debug view shows this peer's own load
		CountVBlank(synced_slow_);
		return;
	}

	if (!config::GdxSlowdown || !gdxsv.IsReplaying() || !Measure(entries_, load_)) {
		if (in_battle_) EndBattle();
		return;
	}
	in_battle_ = true;

	// Over the threshold the game runs at 30fps: every other vblank delivers no input.
	const bool slow = config::GdxSlowdownThreshold <= load_.value;
	stalling_ = slow && !stalling_;
	CountVBlank(slow);
}

bool GdxsvSlowdown::LocalSlow() {
	if (!config::GdxSlowdown || !Measure(entries_, load_)) return false;
	return config::GdxSlowdownThreshold <= load_.value;
}

void GdxsvSlowdown::CountVBlank(bool slow) {
	battle_vblanks_++;
	slow_vblanks_ += slow;
	slow_runs_ += slow && !slow_;
	slow_ = slow;
}

void GdxsvSlowdown::EndBattle() {
	if (0 < battle_vblanks_) {
		NOTICE_LOG(COMMON, "slowdown: battle %d vblanks, 30fps %.1f%% (%d runs, avg %.2fs), threshold %.2f", battle_vblanks_,
				   100.f * slow_vblanks_ / battle_vblanks_, slow_runs_, slow_runs_ ? slow_vblanks_ / 60.f / slow_runs_ : 0.f,
				   config::GdxSlowdownThreshold.get());
	}
	in_battle_ = false;
	stalling_ = false;
	synced_slow_ = false;
	load_ = Load{};
	battle_vblanks_ = slow_vblanks_ = slow_runs_ = 0;
	slow_ = false;
}

void GdxsvSlowdown::DisplayOSD() {
	// Debug view, with the projectile list: hidden option gdxsv:ProjectileView.
	if (!config::GdxProjectileView || !in_battle_) return;

	ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 8, 8), ImGuiCond_Always, ImVec2(1, 0));
	ImGui::SetNextWindowBgAlpha(0.6f);
	ImGui::Begin("##gdxsv_slowdown", nullptr,
				 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
					 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
	// Rollback: the synced state (peer 0's decision); the load shown is this peer's own view.
	const bool slow = ggpo::active() ? synced_slow_ : config::GdxSlowdownThreshold <= load_.value;
	ImGui::TextColored(slow ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(1, 1, 1, 1), "Slowdown load %5.2f / %.2f  %s", load_.value,
					   config::GdxSlowdownThreshold.get(), slow ? "30fps" : "60fps");
	if (0 < battle_vblanks_) {
		ImGui::Text("30fps %4.1f%% of battle, %d runs", 100.f * slow_vblanks_ / battle_vblanks_, slow_runs_);
	}
	for (int c = 0; c < NumCategories; c++) {
		if (load_.counts[c] == 0) continue;
		ImGui::Text("  %-14s %3d x %.2f", CategoryName(c), load_.counts[c], Weight(c));
	}

	// Weapon objects no category counts, to find what is missing. Held weapons (kind 0, and the
	// heat rod itself: kind 14 type 00) are left out on purpose.
	std::map<std::tuple<int, int, int>, int> missing;  // (kind, type, owner MS) -> count
	if (GdxsvProjectileView::Collect(osd_entries_, false)) {
		for (const auto& e : osd_entries_) {
			if (e.kind == 0 || (e.kind == 14 && e.type == 0x00) || 0 <= Classify(e)) continue;
			missing[{e.kind, e.type, e.owner_ms}]++;
		}
	}
	if (!missing.empty()) {
		ImGui::Separator();
		ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Not counted:");
		for (const auto& [key, n] : missing) {
			const auto [kind, type, ms] = key;
			ImGui::Text("  kind %2d type %02x  %-14s x%d", kind, type, GdxsvProjectileView::MsName(ms), n);
		}
	}
	ImGui::End();
}
