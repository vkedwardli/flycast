#include "gdxsv_slowdown.h"

#include "cfg/option.h"
#include "gdxsv.h"
#include "imgui/imgui.h"
#include "network/ggpo.h"

namespace {
constexpr u8 kWeaponClass = 0x11;
constexpr int kMsGouf = 6;
constexpr int kMsGogg = 12;
}  // namespace

// Weapon objects (class 0x11) by kind, measured with a pool + ammo probe on 5 replays.
// Explosions of bazooka shells and crackers are kind 6 objects of their own, so they get
// the blast weights of ANALYSIS.md 2.2.1 instead of a fixed lifetime after the shot.
int GdxsvSlowdown::Classify(const GdxsvProjectileView::Entry& e) {
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
		case 3:	 // cannon shells (Guncannon, GT Gundam)
		case 5:	 // bazooka shells (Char's Zaku, Dom)
			return Bazooka;
		case 7:	  // Char's Zaku cracker
		case 29:  // Zaku cracker
			return Cracker;
		case 6:	 // blast: 0a = cracker, others = bazooka shell / torpedo hit
			if (e.type == 0x0a) return CrackerBlast;
			if (e.owner_ms == kMsGogg) return -1;  // Gogg torpedo hit: covered by the missile weight
			return BazookaBlast;
		default:
			return -1;
	}
}

const char* GdxsvSlowdown::CategoryName(int category) {
	static const char* names[NumCategories] = {"Missile", "Gun", "Bazooka", "Bazooka blast", "Cracker", "Cracker blast", "Beam", "Heat rod"};
	return 0 <= category && category < NumCategories ? names[category] : "?";
}

float GdxsvSlowdown::Weight(int category) {
	static const float weights[NumCategories] = {1.24f, 0.9f, 0.3f, 0.6f, 0.1f, 0.8f, 1.0f, 0.88f};
	return 0 <= category && category < NumCategories ? weights[category] : 0.f;
}

bool GdxsvSlowdown::Measure(std::vector<GdxsvProjectileView::Entry>& scratch, Load& out) {
	out = Load{};
	if (!GdxsvProjectileView::Collect(scratch, false)) return false;
	std::array<bool, 4> heat_rod{};
	for (const auto& e : scratch) {
		const int c = Classify(e);
		if (0 <= c) out.counts[c]++;
		// Kind 14: type 00 is the rod, held all battle; type 01 is the 8-object burst while it is used.
		// One heat rod per Gouf, however many objects the burst has.
		if (e.kind == 14 && e.type == 0x01 && e.owner_ms == kMsGouf && 0 <= e.owner) heat_rod[e.owner] = true;
	}
	for (bool b : heat_rod) out.counts[HeatRod] += b;
	for (int c = 0; c < NumCategories; c++) out.value += Weight(c) * out.counts[c];
	return true;
}

void GdxsvSlowdown::OnVBlank() {
	const bool rollback = ggpo::active();
	if (!config::GdxSlowdown || !(gdxsv.IsReplaying() || rollback)) {
		if (in_battle_) EndBattle();
		return;
	}
	// Rollback decides in StallFrame; a rollback pass must not count twice.
	if (rollback) {
		if (ggpo::isInRollback()) return;
		if (!GdxsvProjectileView::Collect(entries_, false)) {
			if (in_battle_) EndBattle();
			return;
		}
		in_battle_ = true;
		CountVBlank(config::GdxSlowdownThreshold <= load_.value);
		return;
	}

	if (!Measure(entries_, load_)) {
		if (in_battle_) EndBattle();
		return;
	}
	in_battle_ = true;

	// Over the threshold the game runs at 30fps: every other vblank delivers no input.
	const bool slow = config::GdxSlowdownThreshold <= load_.value;
	stalling_ = slow && !stalling_;
	CountVBlank(slow);
}

bool GdxsvSlowdown::StallFrame(int frame) {
	if (!config::GdxSlowdown) return false;
	Load load;
	if (!Measure(entries_, load)) return false;
	const bool stall = frame % 2 == 1 && config::GdxSlowdownThreshold <= load.value;
	if (ggpo::isInRollback()) {
		rollback_decisions_++;
		rollback_stalls_ += stall;
	} else {
		load_ = load;
	}
	return stall;
}

void GdxsvSlowdown::CountVBlank(bool slow) {
	battle_vblanks_++;
	slow_vblanks_ += slow;
	slow_runs_ += slow && !slow_;
	slow_ = slow;
}

void GdxsvSlowdown::EndBattle() {
	if (0 < battle_vblanks_) {
		NOTICE_LOG(COMMON, "slowdown: battle %d vblanks, 30fps %.1f%% (%d runs, avg %.2fs), threshold %.2f, rollback redid %d decisions (%d stalls)",
				   battle_vblanks_, 100.f * slow_vblanks_ / battle_vblanks_, slow_runs_, slow_runs_ ? slow_vblanks_ / 60.f / slow_runs_ : 0.f,
				   config::GdxSlowdownThreshold.get(), rollback_decisions_, rollback_stalls_);
	}
	in_battle_ = false;
	stalling_ = false;
	load_ = Load{};
	battle_vblanks_ = slow_vblanks_ = slow_runs_ = rollback_decisions_ = rollback_stalls_ = 0;
	slow_ = false;
}

void GdxsvSlowdown::DisplayOSD() {
	if (!config::GdxSlowdown || !in_battle_) return;

	ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 8, 8), ImGuiCond_Always, ImVec2(1, 0));
	ImGui::SetNextWindowBgAlpha(0.6f);
	ImGui::Begin("##gdxsv_slowdown", nullptr,
				 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
					 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
	const bool slow = config::GdxSlowdownThreshold <= load_.value;
	ImGui::TextColored(slow ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(1, 1, 1, 1), "Slowdown load %5.2f / %.2f  %s", load_.value,
					   config::GdxSlowdownThreshold.get(), slow ? "30fps" : "60fps");
	if (0 < battle_vblanks_) {
		ImGui::Text("30fps %4.1f%% of battle, %d runs", 100.f * slow_vblanks_ / battle_vblanks_, slow_runs_);
	}
	for (int c = 0; c < NumCategories; c++) {
		if (load_.counts[c] == 0) continue;
		ImGui::Text("  %-14s %3d x %.2f", CategoryName(c), load_.counts[c], Weight(c));
	}
	ImGui::End();
}
