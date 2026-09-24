#include "gdxsv_projectile_view.h"

#include <cstring>

#include "cfg/option.h"
#include "gdxsv.h"
#include "imgui/imgui.h"
#include "libs.h"

namespace {
// DC2 entity pool (init 0c0531ee): 320 slots of 0x210 bytes, descriptor right after the last slot.
constexpr u32 kPoolBase = 0x0c3fba80;
constexpr u32 kPoolCount = 320;
constexpr u32 kPoolStride = 0x210;
constexpr u32 kFreeSlotMark = 0x100;
constexpr u8 kWeaponClass = 0x11;

// Player work blocks.
constexpr u32 kPlayerWorkBase = 0x0c3d1cd4;
constexpr u32 kPlayerWorkStride = 0x2000;
constexpr u32 kPlayerMsIdOffset = 0x1f02;
constexpr u32 kPlayerSceneOffset = 0x5;
constexpr u8 kSceneBattle = 3;

int ownerPlayer(u32 ptr) {
	if (ptr == 0) return -1;
	ptr = (ptr & 0x1fffffff) | 0x0c000000;
	if (ptr < kPlayerWorkBase || ptr >= kPlayerWorkBase + 4 * kPlayerWorkStride) return -1;
	if ((ptr - kPlayerWorkBase) % kPlayerWorkStride != 0) return -1;
	return (ptr - kPlayerWorkBase) / kPlayerWorkStride;
}

float readFloat(u32 addr) {
	u32 v = gdxsv_ReadMem32(addr);
	float f;
	std::memcpy(&f, &v, sizeof(f));
	return f;
}
}  // namespace

const char* GdxsvProjectileView::KindName(u8 kind) {
	// Weapon object kinds (class 0x11). Names from which MS own them over 5 replays
	// (gdxsv-reveng dc2/entity_pool.md); the rest are shown by id only.
	switch (kind) {
		case 0:
			return "Melee weapon (held)";
		case 1:
			return "Beam";
		case 2:
			return "Bullet / missile";
		case 3:
			return "Shell / cannon";
		case 5:
			return "Bazooka";
		case 6:
			return "Explosion puff / fragment";
		default:
			return "?";
	}
}

const char* GdxsvProjectileView::MsName(int ms_id) {
	static const char* names[] = {
		"Gundam", "Guncannon", "GM",	  "Old Zaku", "Zaku",	"Char's Zaku", "Gouf",	 "Dom",	  "Rick Dom", "Gelgoog", "Char's Gelgoog",
		"Gyan",	  "Gogg",	   "Acguy", "Z'Gok",	  "Char's Z'Gok", "Zock",	   "Guntank", "Zeong", "GT Gundam", "GT GM",
	};
	if (0 <= ms_id && ms_id < (int)(sizeof(names) / sizeof(names[0]))) return names[ms_id];
	return "?";
}

bool GdxsvProjectileView::Collect(std::vector<Entry>& out, bool all_classes) {
	out.clear();
	if (gdxsv.Disk() != 2) return false;
	if (gdxsv_ReadMem8(kPlayerWorkBase + kPlayerSceneOffset) != kSceneBattle) return false;

	for (u32 i = 0; i < kPoolCount; i++) {
		const u32 addr = kPoolBase + i * kPoolStride;
		if (gdxsv_ReadMem32(addr + 0x8) == kFreeSlotMark) continue;
		const u32 header = gdxsv_ReadMem32(addr);
		Entry e{};
		e.slot = i;
		e.cls = (header >> 8) & 0xff;
		e.kind = (header >> 16) & 0xff;
		e.type = (header >> 24) & 0xff;
		if (!all_classes && e.cls != kWeaponClass) continue;
		// Weapon objects keep their owner at +0xf8, other classes at +0x18.
		e.owner = ownerPlayer(gdxsv_ReadMem32(addr + (e.cls == kWeaponClass ? 0xf8 : 0x18)));
		e.owner_ms = e.owner < 0 ? -1 : gdxsv_ReadMem8(kPlayerWorkBase + e.owner * kPlayerWorkStride + kPlayerMsIdOffset);
		for (int k = 0; k < 3; k++) e.pos[k] = readFloat(addr + 0x20 + k * 4);
		out.push_back(e);
	}
	return true;
}

void GdxsvProjectileView::DisplayOSD() {
	if (!config::GdxProjectileView) return;
	if (!Collect(entries_, false)) return;

	ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.6f);
	ImGui::Begin("##gdxsv_projectile_view", nullptr,
				 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
					 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
	ImGui::Text("Projectiles: %d", (int)entries_.size());
	for (int kind = 0; kind < 256; kind++) {
		int n = 0;
		for (const auto& e : entries_) n += e.kind == kind;
		if (n == 0) continue;
		ImGui::Separator();
		ImGui::Text("kind %d: %s  x%d", kind, KindName(kind), n);
		// One row per (type, owner); repeated shots (e.g. machine-gun bullets) show as one row with a count.
		std::vector<bool> shown(entries_.size());
		for (size_t i = 0; i < entries_.size(); i++) {
			const auto& e = entries_[i];
			if (e.kind != kind || shown[i]) continue;
			int count = 0;
			for (size_t j = i; j < entries_.size(); j++) {
				const auto& f = entries_[j];
				if (f.kind == kind && f.type == e.type && f.owner == e.owner) {
					shown[j] = true;
					count++;
				}
			}
			char ms[24];
			if (e.owner < 0) {
				snprintf(ms, sizeof(ms), "owner ?");
			} else if (std::strcmp(MsName(e.owner_ms), "?") == 0) {
				snprintf(ms, sizeof(ms), "P%d MS id %d", e.owner + 1, e.owner_ms);
			} else {
				snprintf(ms, sizeof(ms), "P%d %s", e.owner + 1, MsName(e.owner_ms));
			}
			if (count == 1) {
				ImGui::Text("  #%03d type %02x  %-18s (%7.0f %7.0f %7.0f)", e.slot, e.type, ms, e.pos[0], e.pos[1], e.pos[2]);
			} else {
				ImGui::Text("  #%03d type %02x  %-18s x%d", e.slot, e.type, ms, count);
			}
		}
	}
	ImGui::End();
}
