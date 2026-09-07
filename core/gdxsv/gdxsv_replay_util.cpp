#include "gdxsv_replay_util.h"
#include "gdxsv_translation.h"

#include <sys/stat.h>
#include <sys/types.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef _WIN32
#define stat _stat
#define WIN32_LEAN_AND_MEAN
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on
#endif

#include "dirent.h"
#include "gdxsv.h"
#include "json.hpp"
#include "libs.h"
#ifdef _WIN32
#undef stat  // Undefine the macro before including directory.h to use flycast::stat()
#endif
#include "oslib/directory.h"
#include "oslib/oslib.h"
#include "oslib/http_client.h"
#include "ui/gui_util.h"
#include "ui/IconsFontAwesome6.h"
#include "stdclass.h"

#include <sstream>
#include <thread>
#include <algorithm>
#include <chrono>
#include <map>

// For macOS
std::string os_PrecomposedString(std::string string);

namespace {

static const char* ms_names(int id) {
	return GdxsvLanguage::GetMSName(id);
}

std::vector<int> parse_csv_ints(const std::string& s) {
	std::vector<int> result;
	if (s.empty()) return result;
	std::istringstream iss(s);
	std::string token;
	while (std::getline(iss, token, ',')) {
		result.push_back(std::atoi(token.c_str()));
	}
	return result;
}

struct UserEntry {
	std::string user_id;
	std::string name;
	int battle_count;
	int win_count;
	int lose_count;
	int kill_count;
	int renpo_battle_count;
	int renpo_win_count;
	int renpo_lose_count;
	int renpo_kill_count;
	int zeon_battle_count;
	int zeon_win_count;
	int zeon_lose_count;
	int zeon_kill_count;
	int daily_battle_count;
	int daily_win_count;
	int daily_lose_count;
};

struct ReplayEntry {
	std::string disk;
	std::vector<proto::BattleLogUser> users;
	int round = 0;
	int renpo_win = 0;
	int zeon_win = 0;
	time_t start_unix;
	std::string replay_url;
	std::string round_win;
	std::vector<std::string> user_used_ms_list;
	int play_count = 0;
};

bool read_dir = false;
std::vector<std::pair<std::string, uint64_t>> files;
std::string selected_replay_file;
std::string battle_log_file_name;
std::string broken_replay_path;
proto::BattleLogFile battle_log;

std::string search_user_id;
std::string search_user_name;
std::string search_pilot_name;
std::string search_lobby_id;
unsigned int search_no_of_players;
std::string search_battle_code;
int search_ranking = -1;
std::string search_disk;
bool search_reverse;
int search_used_ms = -1;

std::shared_future<std::vector<UserEntry>> fetch_user_entry_future_;
int fetch_user_entry_http_status;

std::shared_future<std::vector<ReplayEntry>> fetch_replay_entry_future_;
int fetch_replay_entry_http_status;

// One in-progress battle from /lbs/status, joining active_games with the
// battle_users that belong to it.
struct LiveEntry {
	std::string battle_code;
	std::string disk;
	std::string state;
	int lobby_id = 0;
	bool live_spectate = false;
	int spectators = 0;
	time_t updated_unix = 0;
	std::vector<proto::BattleLogUser> users;
};

std::shared_future<std::vector<LiveEntry>> fetch_live_entry_future_;
int fetch_live_entry_http_status;
std::chrono::steady_clock::time_point live_last_fetch_;

// The last good result stays on screen while the next fetch is in flight -
// otherwise the list blinks out every refresh. Only the very first load has
// nothing to show.
std::vector<LiveEntry> live_entries_;
bool live_first_load_ = true;

// By battle_code, not by index: battles come and go between refreshes, so an
// index would silently end up pointing at a different battle.
std::string selected_live_battle_code;

// Battles start and end while the tab is open, so the list refreshes itself.
// /lbs/status collapses concurrent requests with singleflight, so this is
// cheap on the server; polling only runs while the tab is actually visible.
constexpr int kLiveRefreshSeconds = 10;
int selected_replay_entry_index = -1;

int entry_paging = 0;
char page_buf[4] = "1";
int pov_index = -1;
ImVec2 normal_padding;
float scaling;

const std::array<std::array<const char*, 2>, 17> lobby_data{{{"Taklamakan Desert", "2"},
															 {"Black Sea Forest", "4"},
															 {"Odessa", "5"},
															 {"Belfast", "6"},
															 {"New York", "9"},
															 {"Grand Canyon", "10"},
															 {"Jaburo", "11"},
															 {"UG Complex", "12"},
															 {"Solomon", "13"},
															 {"Solomon (Space)", "14"},
															 {"A Baoa Qu (Space)", "15"},
															 {"A Baoa Qu (Outter)", "16"},
															 {"A Baoa Qu (Inner)", "17"},
															 {"Sat.Orbit 1", "19"},
															 {"Sat.Orbit 2", "20"},
															 {"SIDE 6 (Space)", "21"},
															 {"SIDE 7 (Inner)", "22"}}};

// active_games carries no start time, only when the record was last touched -
// which for a battle still in progress is when it started.
time_t parse_iso8601(const std::string& v) {
	std::tm tm{};
	int frac = 0;
	char sign = '+';
	int off_h = 0, off_m = 0;
	if (sscanf(v.c_str(), "%d-%d-%dT%d:%d:%d.%d%c%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &frac,
			   &sign, &off_h, &off_m) < 6) {
		return 0;
	}
	tm.tm_year -= 1900;
	tm.tm_mon -= 1;
	// timegm is POSIX. Windows spells it _mkgmtime, in both MinGW and MSVC.
#ifdef _WIN32
	const time_t utc = _mkgmtime(&tm);
#else
	const time_t utc = timegm(&tm);
#endif
	const time_t offset = (off_h * 3600 + off_m * 60) * (sign == '-' ? -1 : 1);
	return utc - offset;
}

// "dc1"/"dc2" are internal names. Players call the two discs MUJI and DX, which
// is what the Server tab's own disk filter buttons already say.
const char* disk_display_name(const std::string& disk) {
	if (disk == "dc1") return "MUJI";
	if (disk == "dc2") return "DX";
	return disk.c_str();
}

// Translated, so the Live list reads the same as the Server tab's filter.
const char* lobby_name_by_id(int lobby_id) {
	const auto id = std::to_string(lobby_id);
	for (const auto& row : lobby_data) {
		if (id == row[1]) return GdxsvLanguage::gdxT(row[0]);
	}
	return nullptr;
}

void textCentered(const std::string& text) {
	auto windowWidth = ImGui::GetWindowSize().x;
	auto textWidth = ImGui::CalcTextSize(text.c_str()).x;
	ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
	ImGui::Text(text.c_str());
};

void gdxsv_replay_draw_forces(const bool is_renpo, const std::vector<int>& force_index, int& user_index,
							  const std::vector<proto::BattleLogUser>& users) {
	ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2.0f * scaling);

	if (is_renpo) {
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(.42f, .79f, .99f, 1));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(.055f, .122f, .227f, .3f));
	} else {
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(.97f, .23f, .35f, 1));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(.196f, .07f, .05f, .3f));
	}

	for (int i : force_index) {
		if (i != force_index.front()) ImGui::SameLine();
		auto pos = ImGui::GetCursorPos();
		if (ImGui::Selectable(("##pov_" + std::to_string(user_index)).c_str(), (pov_index == user_index), 0, ScaledVec2(180, 90))) {
			if (pov_index == user_index) {
				pov_index = -1;
			} else {
				pov_index = user_index;
			}
		}
		ImGui::SetCursorPos(ImVec2(pos.x, pos.y));
		ImGui::BeginChild(ImGui::GetID(("gdxsv_replay_file_detail_renpo_" + std::to_string(i)).c_str()), ScaledVec2(180, 90), true,
						  ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs);
		textCentered("ID: " + users[user_index].user_id());
		textCentered("HN: " + users[user_index].user_name());
		textCentered("PN: " + users[user_index].pilot_name());
		ImGui::EndChild();
		user_index++;
	}
	ImGui::PopStyleColor();
	ImGui::PopStyleColor();

	ImGui::PopStyleVar();
}

void gdxsv_replay_draw_players(const std::vector<proto::BattleLogUser>& users) {
	std::vector<int> renpo_index, zeon_index;
	for (int i = 0; i < users.size(); i++) {
		if (users[i].team() == 1) renpo_index.push_back(i);
		if (users[i].team() == 2) zeon_index.push_back(i);
	}
	int user_index = 0;

	gdxsv_replay_draw_forces(true, renpo_index, user_index, users);
	gdxsv_replay_draw_forces(false, zeon_index, user_index, users);
}

void gdxsv_replay_draw_info(const std::string& battle_code, const std::string& game_disk, const int& users_size,
							const std::string& close_reason, const time_t& start_time, const time_t& end_time,
							const std::vector<proto::BattleLogUser>& users, const std::string& replay_dst,
							int play_count = -1) {
	const bool playable = "dc" + std::to_string(gdxsv.Disk()) == game_disk;

	// Player cards + Replay button first
	gdxsv_replay_draw_players(users);

	ImGui::NewLine();

	{
		bool pov_selected = (pov_index == -1);
		ImGui::BeginDisabled(pov_selected || !playable);

		if (ImGui::ButtonEx(pov_selected ? ICON_FA_ARROW_POINTER "  Select a player" : ICON_FA_PLAY "  Replay", ScaledVec2(240, 50))) {
			gdxsv_start_replay(replay_dst, pov_index);
		}

		ImGui::EndDisabled();

		if (!broken_replay_path.empty() && broken_replay_path == replay_dst) {
			ImGui::Text("Failed to start replay. The replay file is corrupted or outdated.");
		}
	}

	ImGui::NewLine();

	// Details below
	ImGui::Text("BattleCode: %s", battle_code.c_str());
	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_CLIPBOARD "  Copy")) {
		ImGui::SetClipboardText(battle_code.c_str());
	}
	ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(0, -13.0) * scaling);
	ImGui::Text("Game: %s", disk_display_name(game_disk));
	if (play_count >= 0) {
		ImGui::Text("Views: %d", play_count);
	}

	char buf[128] = {0};
	if (start_time != 0 && std::localtime(&start_time) != nullptr) {
		std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&start_time));
		ImGui::Text("StartAt: %s", buf);
	}
	if (end_time != 0 && std::localtime(&end_time) != nullptr) {
		std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&end_time));
		ImGui::Text("EndAt: %s", buf);
	}
	if (!close_reason.empty()) {
		ImGui::Text("CloseReason: %s", close_reason.c_str());
	}
	OptionCheckbox("Hide name", config::GdxReplayHideName, "Replace player names with generic names");
	OptionCheckbox("Show Ally HP", config::GdxReplayShowAllyHP, "Hack the total HP field to display Ally HP");
	OptionCheckbox("Key Display", config::GdxReplayKeyDisplay, "Display controller inputs");
	OptionCheckbox("Skip MS Selection", config::GdxReplaySkipMsSelection, "Fast-forward through the mobile suit selection screen");
}

void draw_round_detail(const ReplayEntry& entry) {
	auto round_wins = parse_csv_ints(entry.round_win);
	if (round_wins.empty()) return;

	std::vector<std::vector<int>> user_ms_lists;
	for (const auto& ms_str : entry.user_used_ms_list) {
		user_ms_lists.push_back(parse_csv_ints(ms_str));
	}

	ImGui::Separator();
	ImGui::TextDisabled("Round Detail");

	for (int r = 0; r < (int)round_wins.size(); r++) {
		int win_team = round_wins[r];
		ImGui::Text("R%d", r + 1);
		ImGui::SameLine();
		if (win_team == 1) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.42f, .79f, .99f, 1));
			ImGui::Text("%s", GdxsvLanguage::gdxT("Federation"));
		} else if (win_team == 2) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.97f, .23f, .35f, 1));
			ImGui::Text("%s", GdxsvLanguage::gdxT("Zeon"));
		} else {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.5f, .5f, .5f, 1));
			ImGui::Text("  -  ");
		}
		ImGui::PopStyleColor();
		ImGui::SameLine();
		std::string renpo_ms, zeon_ms;
		for (int j = 0; j < (int)entry.users.size(); j++) {
			int ms_id = 0;
			if (j < (int)user_ms_lists.size() && r < (int)user_ms_lists[j].size()) {
				ms_id = user_ms_lists[j][r];
			}
			if (ms_id == 0) continue;
			const char* name = (ms_id >= 1 && ms_id <= 41) ? ms_names(ms_id - 1) : "?";
			if (entry.users[j].team() == 1) {
				if (!renpo_ms.empty()) renpo_ms += ", ";
				renpo_ms += name;
			} else {
				if (!zeon_ms.empty()) zeon_ms += ", ";
				zeon_ms += name;
			}
		}
		if (!renpo_ms.empty() || !zeon_ms.empty()) {
			ImGui::Text("%s vs %s", renpo_ms.c_str(), zeon_ms.c_str());
		}
	}
}

void gdxsv_replay_local_tab() {
	const auto replay_dir = get_writable_data_path("replays");

	if (!read_dir) {
		files.clear();
		read_dir = true;

		if (file_exists(replay_dir)) {
			DIR* dir = flycast::opendir(replay_dir.c_str());

			while (true) {
				struct dirent* entry = flycast::readdir(dir);
				if (entry == nullptr) break;
				std::string name(entry->d_name);
#ifdef __APPLE__
				name = os_PrecomposedString(name);
#endif
				if (name == ".") continue;
				std::string extension = get_file_extension(name);
				if (extension == "pb") {
					struct stat result {};
					if (flycast::stat((replay_dir + "/" + name).c_str(), &result) == 0) {
						files.emplace_back(name, result.st_mtime);
					}
				}
			}
			std::sort(files.begin(), files.end(), std::greater<>());

			flycast::closedir(dir);
		}
	}

	if (ImGui::Button(ICON_FA_ARROW_ROTATE_RIGHT "  Reload")) {
		read_dir = false;
	}

	ImGui::SameLine();
#if defined(TARGET_MAC)
	if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Reveal in Finder")) {
		char temp[512];
		snprintf(temp, sizeof(temp), "open \"%s\"", replay_dir.c_str());
		system(temp);
	}
#elif defined(_WIN32) && !defined(TARGET_UWP)
	if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open folder")) {
		const std::string lpParam = "/root, " + replay_dir;
		SHELLEXECUTEINFOA sei{};
		sei.cbSize = sizeof(sei);
		sei.fMask = SEE_MASK_NOCLOSEPROCESS;
		sei.lpFile = "Explorer.exe";
		sei.lpParameters = lpParam.c_str();
		sei.nShow = SW_SHOWDEFAULT;
		ShellExecuteExA(&sei);
	}
#endif

	ImGui::SameLine();
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, normal_padding);
	ImVec2 size;
	size.x = ImGui::GetContentRegionAvail().x;
	size.y = (ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().FramePadding.y * 2.f);
	if (ImGui::BeginListBox("##Replay File Directory", size)) {
		ImGui::AlignTextToFramePadding();
		ImGui::Text("%s", get_writable_data_path("replays").c_str());

		ImGui::EndListBox();
	}
	ImGui::PopStyleVar();

	ImGui::BeginChild(ImGui::GetID("gdxsv_replay_file_list"), ImVec2(330, 0) * scaling, true, ImGuiWindowFlags_DragScrolling);
	{
		if (files.empty()) {
			ImGui::Text("(No replay found)");
		} else {
			for (int i = 0; i < files.size(); ++i) {
				ImGui::PushID(i);
				if (ImGui::Selectable(files[i].first.c_str(), files[i].first == selected_replay_file, 0, ImVec2(0, 0))) {
					selected_replay_file = files[i].first;
				}
				ImGui::SameLine();

				time_t t = files[i].second;
				char buf[128] = {0};
				std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
				ImGui::Text(buf);
				ImGui::PopID();
			}
		}
	}
	scrollWhenDraggingOnVoid();
	windowDragScroll();
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginChild(ImGui::GetID("gdxsv_replay_file_detail"), ImVec2(0, 0), true, ImGuiWindowFlags_DragScrolling);
	{
		if (!selected_replay_file.empty()) {
			const auto replay_file_path = replay_dir + "/" + selected_replay_file;
			if (battle_log_file_name != selected_replay_file) {
				battle_log_file_name = selected_replay_file;
				battle_log.Clear();
				FILE* fp = nowide::fopen(replay_file_path.c_str(), "rb");
				if (fp != nullptr) {
					battle_log.ParseFromFileDescriptor(fileno(fp));
					std::fclose(fp);
				}
				pov_index = -1;
			}

			gdxsv_replay_draw_info(battle_log.battle_code(), battle_log.game_disk(), battle_log.users_size(), battle_log.close_reason(),
								   battle_log.start_at(), battle_log.end_at(),
								   std::vector<proto::BattleLogUser>(battle_log.users().begin(), battle_log.users().end()),
								   replay_dir + "/" + selected_replay_file);
		}
	}
	scrollWhenDraggingOnVoid();
	windowDragScroll();
	ImGui::EndChild();
}

void parse_replay_json(const std::vector<u8>& json_string, std::vector<ReplayEntry>& out) {
	try {
		nlohmann::json j = nlohmann::json::parse(json_string);

		for (auto item : j) {
			ReplayEntry entry;
			entry.disk = item.at("disk");

			for (auto u : item.at("users")) {
				auto user = proto::BattleLogUser();
				user.set_user_id(u.at("user_id"));
				user.set_user_name(u.at("user_name"));
				user.set_pilot_name(u.at("pilot_name"));
				user.set_team(u.at("team"));
				user.set_pos(u.at("pos"));
				entry.users.push_back(user);
				entry.user_used_ms_list.push_back(u.value("used_ms_list", ""));
			}

			entry.round = item.value("round", 0);
			entry.renpo_win = item.value("renpo_win", 0);
			entry.zeon_win = item.value("zeon_win", 0);
			entry.start_unix = item.at("start_unix");
			entry.replay_url = item.at("replay_url");
			entry.round_win = item.value("round_win", "");
			entry.play_count = item.value("play_count", 0);

			out.push_back(entry);
		}
	} catch (const nlohmann::json::exception& e) {
		WARN_LOG(COMMON, "json parse failure: %s", e.what());
	}
}

void parse_user_json(const std::vector<u8>& json_string, std::vector<UserEntry>& out) {
	try {
		nlohmann::json j = nlohmann::json::parse(json_string);

		for (auto item : j) {
			UserEntry entry;
			entry.user_id = item.value("user_id", "");
			entry.name = item.value("name", "");
			entry.battle_count = item.value("battle_count", -1);
			entry.win_count = item.value("win_count", -1);
			entry.lose_count = item.value("lose_count", -1);
			entry.kill_count = item.value("kill_count", -1);
			entry.renpo_battle_count = item.value("renpo_battle_count", -1);
			entry.renpo_win_count = item.value("renpo_win_count", -1);
			entry.renpo_lose_count = item.value("renpo_lose_count", -1);
			entry.renpo_kill_count = item.value("renpo_kill_count", -1);
			entry.zeon_battle_count = item.value("zeon_battle_count", -1);
			entry.zeon_win_count = item.value("zeon_win_count", -1);
			entry.zeon_lose_count = item.value("zeon_lose_count", -1);
			entry.zeon_kill_count = item.value("zeon_kill_count", -1);
			entry.daily_battle_count = item.value("daily_battle_count", -1);
			entry.daily_win_count = item.value("daily_win_count", -1);
			entry.daily_lose_count = item.value("daily_lose_count", -1);
			out.push_back(entry);
		}
	} catch (const nlohmann::json::exception& e) {
		WARN_LOG(COMMON, "json parse failure: %s", e.what());
	}
}

void fetch_replay_json() {
	if (fetch_replay_entry_future_.valid()) {
		return;
	}

	const auto future_fn = []() -> std::vector<ReplayEntry> {
		std::vector<ReplayEntry> entries{};
		std::vector<u8> dl;
		std::string content_type;
		http::init();
		std::string url = "https://asia-northeast1-gdxsv-274515.cloudfunctions.net/lbsapi/replay?";

		url += "page=" + http::urlEncode(std::to_string(entry_paging));
		if (!search_user_id.empty()) {
			url += "&user_id=" + http::urlEncode(search_user_id);
		}
		if (!search_user_name.empty()) {
			url += "&user_name=" + http::urlEncode("%" + search_user_name + "%");
		}
		if (!search_pilot_name.empty()) {
			url += "&pilot_name=" + http::urlEncode("%" + search_pilot_name + "%");
		}
		if (!search_lobby_id.empty()) {
			url += "&lobby_id=" + http::urlEncode(search_lobby_id);
		}
		if (search_no_of_players != 0) {
			url += "&players=" + http::urlEncode(std::to_string(search_no_of_players));
		}
		if (!search_battle_code.empty()) {
			url += "&battle_code=" + http::urlEncode(search_battle_code);
		}
		if (search_ranking != -1) {
			url += "&aggregate=" + http::urlEncode(std::to_string(search_ranking));
		}
		if (!search_disk.empty()) {
			url += "&disk=" + http::urlEncode(search_disk);
		}
		if (search_reverse) {
			url += "&reverse=" + http::urlEncode(std::to_string(1));
		}
		if (search_used_ms != -1) {
			url += "&used_ms=" + http::urlEncode(std::to_string(search_used_ms));
		}

		fetch_replay_entry_http_status = http::get(url, dl, content_type);
		if (fetch_replay_entry_http_status != 200) {
			ERROR_LOG(COMMON, "version check failure: %s", url.c_str());
			return entries;
		}

		parse_replay_json(dl, entries);
		return entries;
	};

	fetch_replay_entry_future_ = std::async(std::launch::async, future_fn).share();
}

void parse_live_json(const std::vector<u8>& json_string, std::vector<LiveEntry>& out) {
	try {
		nlohmann::json j = nlohmann::json::parse(json_string);

		std::map<std::string, LiveEntry> by_code;
		for (const auto& g : j.value("active_games", nlohmann::json::array())) {
			LiveEntry e;
			e.battle_code = g.value("battle_code", "");
			if (e.battle_code.empty()) continue;
			e.disk = g.value("disk", "");
			e.state = g.value("state", "");
			e.lobby_id = g.value("lobby_id", 0);
			e.live_spectate = g.value("live_spectate", false);
			e.spectators = g.value("spectators", 0);
			e.updated_unix = parse_iso8601(g.value("updated_at", ""));
			by_code[e.battle_code] = e;
		}

		// battle_users is a flat list across every battle, and the server
		// dedups it by user_id - so a player in two battles appears once and
		// one of those battles comes up a man short. Render what arrived.
		for (const auto& u : j.value("battle_users", nlohmann::json::array())) {
			auto it = by_code.find(u.value("battle_code", ""));
			if (it == by_code.end()) continue;
			auto user = proto::BattleLogUser();
			user.set_user_id(u.value("user_id", ""));
			user.set_user_name(u.value("name", ""));
			user.set_pilot_name(u.value("pilot_name", ""));
			user.set_pos(u.value("battle_pos", 0));
			user.set_team(u.value("team", "") == "renpo" ? 1 : 2);
			it->second.users.push_back(user);
		}

		for (auto& kv : by_code) {
			std::sort(kv.second.users.begin(), kv.second.users.end(),
					  [](const proto::BattleLogUser& a, const proto::BattleLogUser& b) { return a.pos() < b.pos(); });
			out.push_back(kv.second);
		}
	} catch (const nlohmann::json::exception& e) {
		ERROR_LOG(COMMON, "live status json parse failure: %s", e.what());
	}
}

void fetch_live_json() {
	if (fetch_live_entry_future_.valid()) {
		return;
	}
	live_last_fetch_ = std::chrono::steady_clock::now();

	const auto future_fn = []() -> std::vector<LiveEntry> {
		std::vector<LiveEntry> entries{};
		std::vector<u8> dl;
		std::string content_type;
		http::init();
		const std::string url = config::loadStr("gdxsv", "LiveApiUrl", "https://asia-northeast1-gdxsv-274515.cloudfunctions.net/lbsapi") + "/status";

		fetch_live_entry_http_status = http::get(url, dl, content_type);
		if (fetch_live_entry_http_status != 200) {
			ERROR_LOG(COMMON, "live status fetch failure %d: %s", fetch_live_entry_http_status, url.c_str());
			return entries;
		}

		parse_live_json(dl, entries);
		return entries;
	};

	fetch_live_entry_future_ = std::async(std::launch::async, future_fn).share();
}

void fetch_live_refresh() {
	fetch_live_entry_future_ = std::shared_future<std::vector<LiveEntry>>();
	fetch_live_json();
}

// Moves a finished fetch into live_entries_ and frees the future for the next
// poll. A failed fetch leaves the previous list up rather than emptying it.
void live_harvest_fetch() {
	if (!fetch_live_entry_future_.valid() || !future_is_ready(fetch_live_entry_future_)) {
		return;
	}
	if (fetch_live_entry_http_status == 200) {
		live_entries_ = fetch_live_entry_future_.get();
	}
	live_first_load_ = false;
	fetch_live_entry_future_ = std::shared_future<std::vector<LiveEntry>>();
}

// Viewer count for a battle being watched right now. Polled over HTTP, and
// only while the control bar is on screen.
std::shared_future<int> fetch_viewers_future_;
std::chrono::steady_clock::time_point viewers_last_fetch_;
int live_viewer_count_ = 0;
constexpr int kViewerRefreshSeconds = 5;

void fetch_user_json() {
	if (fetch_user_entry_future_.valid()) {
		return;
	}

	const auto future_fn = []() -> std::vector<UserEntry> {
		std::vector<UserEntry> entries{};
		std::vector<u8> dl;
		std::string content_type;
		http::init();
		std::string url = "https://asia-northeast1-gdxsv-274515.cloudfunctions.net/lbsapi/user?";

		std::string loginkey = config::loadStr("gdxsv", "loginkey", "");
		std::vector<u8> e_loginkey(loginkey.size());
		static constexpr int magic[] = {0x46, 0xcf, 0x2d, 0x55};
		for (int i = 0; i < e_loginkey.size(); ++i) e_loginkey[i] ^= loginkey[i] ^ magic[i & 3];
		unsigned hash = 2166136261U;
		for (const unsigned char e : e_loginkey) hash = hash * 16777619U ^ static_cast<unsigned>(e);
		std::ostringstream hashed_loginkey_s;
		hashed_loginkey_s << std::setfill('0') << std::setw(8) << std::hex << hash;

		url += "login_key=" + http::urlEncode(hashed_loginkey_s.str());

		fetch_user_entry_http_status = http::get(url, dl, content_type);
		if (fetch_user_entry_http_status != 200) {
			ERROR_LOG(COMMON, "version check failure: %s", url.c_str());
			return entries;
		}

		parse_user_json(dl, entries);
		return entries;
	};

	fetch_user_entry_future_ = std::async(std::launch::async, future_fn).share();
}

void fetch_new_results(bool reset_page = true) {
	if (reset_page) {
		entry_paging = 0;
		snprintf(page_buf, sizeof(page_buf), "%d", 1);
	}
	selected_replay_entry_index = -1;
	fetch_replay_entry_future_ = std::shared_future<std::vector<ReplayEntry>>();
}

void fetch_target_page() { fetch_new_results(false); }

template <typename Callable>
void draw_filter_label(const std::string& label, const std::string& value, Callable on_click) {
	static char filter_user_id_buf[100] = {0};
	snprintf(filter_user_id_buf, sizeof(filter_user_id_buf), u8"%s = %s  ×", label.c_str(), value.c_str());

	ImGui::SameLine();
	ImGui::GetStyle().FrameRounding = 5.0f * scaling;
	if (ImGui::Button(
			filter_user_id_buf,
			ImVec2(ImGui::CalcTextSize(filter_user_id_buf, NULL, true).x + ImGui::GetStyle().FramePadding.x * 2 - 5.0f * scaling, 0))) {
		on_click();
		fetch_new_results();
	}
	ImGui::GetStyle().FrameRounding = 0.0f;
};

void draw_filter_label_string(const std::string& label, std::string& value) {
	if (!value.empty()) {
		draw_filter_label(label, value, [&value]() { value = ""; });
	}
};

void draw_filter_label_int(const std::string& label, unsigned int& value) {
	if (value != 0) {
		draw_filter_label(label, std::to_string(value), [&value]() { value = 0; });
	}
};
void draw_filter_label_yesno(const std::string& label, int& value) {
	if (value != -1) {
		draw_filter_label(label, value ? "Yes" : "No", [&value]() { value = -1; });
	}
};
void draw_filter_label_bool(const std::string& label, bool& value) {
	if (value != false) {
		draw_filter_label(label, "Yes", [&value]() { value = false; });
	}
};

void gdxsv_replay_live_tab() {
	live_harvest_fetch();

	const bool due = kLiveRefreshSeconds <= std::chrono::duration_cast<std::chrono::seconds>(
											   std::chrono::steady_clock::now() - live_last_fetch_)
											   .count();
	if (!fetch_live_entry_future_.valid() && (live_first_load_ || due)) {
		fetch_live_json();
	}

	const bool refreshing = fetch_live_entry_future_.valid();
	{
		DisabledScope scope(refreshing);
		// Default size, like every other button in these tabs - so the status
		// line beside it lands on the same baseline without nudging.
		if (ImGui::Button(ICON_FA_ARROW_ROTATE_RIGHT "  Refresh") && !scope.isDisabled()) {
			fetch_live_refresh();
		}
	}
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	if (refreshing && !live_first_load_) {
		ImGui::TextDisabled("Battles in progress. Updating...");
	} else if (fetch_live_entry_http_status != 200 && !live_first_load_) {
		ImGui::TextDisabled("Battles in progress. Last update failed (HTTP %d).", fetch_live_entry_http_status);
	} else {
		ImGui::TextDisabled("Battles in progress. Updates every %ds.", kLiveRefreshSeconds);
	}

	ImGui::BeginChild(ImGui::GetID("gdxsv_live_list"), ScaledVec2(450, 0), true, ImGuiWindowFlags_DragScrolling);
	{
		if (live_first_load_) {
			ImGui::Text("Loading...");
		} else if (live_entries_.empty()) {
			ImGui::Text("No battle in progress.");
		} else {
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ScaledVec2(0, 30.0f));
			for (int i = 0; i < (int)live_entries_.size(); ++i) {
				ImGui::PushID(i);
				const auto& entry = live_entries_[i];

				const char* row_lobby = lobby_name_by_id(entry.lobby_id);
				std::string row = "  ";
				row += entry.live_spectate ? ICON_FA_TOWER_BROADCAST : ICON_FA_CIRCLE_DOT;
				row += "  ";
				row += row_lobby != nullptr ? std::string(row_lobby) : ("Lobby " + std::to_string(entry.lobby_id));
				row += "\n\n";
				for (int u = 0; u < (int)entry.users.size(); u++) {
					const auto& user = entry.users[u];
					row += user.user_name();
					if (u + 1 < (int)entry.users.size()) {
						row += entry.users[u + 1].team() != user.team() ? " vs " : ", ";
					}
				}

				const ImVec2 row_pos = ImGui::GetCursorScreenPos();

				// Listed but not selectable: everyone in it is on a build
				// without the uplink, so there is nothing to receive.
				if (!entry.live_spectate) {
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
					ImGui::Selectable(row.c_str(), false, ImGuiSelectableFlags_Disabled, ImVec2(0, 0));
					ImGui::PopStyleColor();
				} else if (ImGui::Selectable(row.c_str(), entry.battle_code == selected_live_battle_code, 0, ImVec2(0, 0))) {
					selected_live_battle_code = entry.battle_code;
					pov_index = -1;
				}

				// Viewer count, right-aligned on the lobby line. Drawn straight
				// to the draw list so it does not become a second item and eat
				// clicks meant for the row.
				if (entry.live_spectate) {
					char watchers[64] = {};
					snprintf(watchers, sizeof(watchers), ICON_FA_EYE " %d", entry.spectators);
					const ImVec2 text_size = ImGui::CalcTextSize(watchers);
					const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
					const float right_margin = 8.0f * scaling;
					ImGui::GetWindowDrawList()->AddText(ImVec2(right - text_size.x - right_margin, row_pos.y),
														ImGui::GetColorU32(ImGuiCol_TextDisabled), watchers);
				}

				ImGui::PopID();
			}
			ImGui::PopStyleVar();
		}
		scrollWhenDraggingOnVoid();
		windowDragScroll();
	}
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginChild(ImGui::GetID("gdxsv_live_detail"), ImVec2(0, 0), true, ImGuiWindowFlags_DragScrolling);
	{
		const LiveEntry* selected = nullptr;
		for (const auto& e : live_entries_) {
			if (e.battle_code == selected_live_battle_code) {
				selected = &e;
				break;
			}
		}

		if (selected == nullptr) {
			ImGui::Text(selected_live_battle_code.empty() ? "Select a battle to watch." : "That battle has ended.");
		} else {
			// Same blue-renpo / red-zeon cards the replay detail uses, and the
			// same pov_index, so picking a view works identically in both.
			gdxsv_replay_draw_players(selected->users);

			ImGui::NewLine();
			{
				const bool playable = ("dc" + std::to_string(gdxsv.Disk())) == selected->disk;
				ImGui::BeginDisabled(pov_index == -1 || !playable);
				if (ImGui::ButtonEx(pov_index == -1 ? ICON_FA_ARROW_POINTER "  Select a player" : ICON_FA_TOWER_BROADCAST "  Watch Live",
									ScaledVec2(240, 50))) {
					gdxsv_start_live_spectate(selected->battle_code, pov_index);
				}
				ImGui::EndDisabled();
				if (!playable) {
					ImGui::SameLine();
					ImGui::AlignTextToFramePadding();
					ImGui::TextDisabled("This battle is on %s.", disk_display_name(selected->disk));
				}
			}

			// Same fields the replay detail lists, so the two read alike.
			ImGui::NewLine();
			const char* sel_lobby = lobby_name_by_id(selected->lobby_id);
			if (sel_lobby != nullptr) {
				ImGui::Text("Lobby: %s", sel_lobby);
			} else {
				ImGui::Text("Lobby: %d", selected->lobby_id);
			}
			ImGui::Text("BattleCode: %s", selected->battle_code.c_str());
			ImGui::Text("Game: %s", disk_display_name(selected->disk));
			ImGui::Text("Current Viewers: %d", selected->spectators);
			if (selected->updated_unix != 0) {
				char timebuf[128] = {};
				std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", std::localtime(&selected->updated_unix));
				ImGui::Text("StartAt: %s", timebuf);
			}
		}
		scrollWhenDraggingOnVoid();
		windowDragScroll();
	}
	ImGui::EndChild();
}


void gdxsv_replay_server_tab() {
	struct TextFilters {
		// Return 0 (pass) if the character is number
		static int FilterNumber(ImGuiInputTextCallbackData* data) {
			if (data->EventChar < 256 && strchr("0123456789", (char)data->EventChar)) return 0;
			return 1;
		}

		static int FullWidthAlphaNum(ImGuiInputTextCallbackData* data) {
			if (data->EventChar >= 0x21 && data->EventChar <= 0x7E) data->EventChar = (data->EventChar + 0xFEE0);

			return 0;
		}

		static int UppercaseAlpha(ImGuiInputTextCallbackData* data) {
			if (data->EventChar >= 0x21 && data->EventChar <= 0x7E) data->EventChar = toupper(data->EventChar);

			return 0;
		}
	};

	ImGui::AlignTextToFramePadding();

	ImGui::Text("Filter by");

	const std::array<std::string, 10> filter_labels{"User ID",	  "User Name", "Pilot Name", "Lobby ID",	 "No. of Players",
												    "Battle Code", "Ranking",   "Disk",		 "Used MS", "Reverse Order"};
	static unsigned int filter_selected = 0;

	ImGui::SameLine();
	ImGui::PushItemWidth(150.0f * scaling);
	if (ImGui::BeginCombo("##FilterItems", filter_labels[filter_selected].c_str(), ImGuiComboFlags_HeightLargest)) {
		for (u32 i = 0; i < filter_labels.size(); i++) {
			bool is_selected = i == filter_selected;
			if (ImGui::Selectable(filter_labels[i].c_str(), is_selected)) filter_selected = i;
			if (is_selected) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	ImGui::PopItemWidth();

	{
		DisabledScope loading_scope(!future_is_ready(fetch_replay_entry_future_));

		switch (filter_selected) {
			case 0:	 // User ID
			{
				if (!fetch_user_entry_future_.valid()) {
					fetch_user_json();
				}
				static char user_id_buf[7] = {0};
				{
					DisabledScope loading_scope(!future_is_ready(fetch_user_entry_future_));
					ImGui::SameLine();
					if (ImGui::Button("My IDs")) ImGui::OpenPopup("my_id_popup");
					ImGui::SameLine();
					if (ImGui::BeginPopup("my_id_popup")) {
						ImGui::Text("Handle Name");
						ImGui::Separator();
						auto entries = fetch_user_entry_future_.get();
						for (int i = 0; i < entries.size(); i++)
							if (ImGui::Selectable(entries[i].name.c_str())) {
								snprintf(user_id_buf, sizeof(user_id_buf), "%s", entries[i].user_id.c_str());
							}
						ImGui::EndPopup();
					}
				}

				ImGui::SameLine();
				if (ImGui::InputText("##user_id_input", user_id_buf, IM_ARRAYSIZE(user_id_buf),
									 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter,
									 TextFilters::UppercaseAlpha)) {
					search_user_id = std::string(user_id_buf);
					fetch_new_results();
				}

				ImGui::SameLine();
				if (ImGui::Button("Add Filter")) {
					search_user_id = std::string(user_id_buf);
					fetch_new_results();
				}

				break;
			}
			case 1:	 // User Name
			{
				static char user_name_buf[100] = {0};
				ImGui::SameLine();
				if (ImGui::InputText("##user_name_input", user_name_buf, IM_ARRAYSIZE(user_name_buf),
									 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter,
									 TextFilters::FullWidthAlphaNum)) {
					search_user_name = std::string(user_name_buf);
					fetch_new_results();
				}

				ImGui::SameLine();
				if (ImGui::Button("Add Filter")) {
					search_user_name = std::string(user_name_buf);
					fetch_new_results();
				}

				break;
			}
			case 2:	 // Pilot Name
			{
				static char pilot_name_buf[100] = {0};
				ImGui::SameLine();
				if (ImGui::InputText("##user_name_input", pilot_name_buf, IM_ARRAYSIZE(pilot_name_buf),
									 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter,
									 TextFilters::FullWidthAlphaNum)) {
					search_pilot_name = std::string(pilot_name_buf);
					fetch_new_results();
				}

				ImGui::SameLine();
				if (ImGui::Button("Add Filter")) {
					search_pilot_name = std::string(pilot_name_buf);
					fetch_new_results();
				}

				break;
			}
			case 3:	 // Lobby ID
			{
				static unsigned int lobby_selected = 0;

				auto get_lobby_name = [&](size_t i) {
					return GdxsvLanguage::gdxT(lobby_data[i][0]);
				};

				ImGui::SameLine();
				ImGui::PushItemWidth(300.0f * scaling);
				if (ImGui::BeginCombo("##LobbyItems", get_lobby_name(lobby_selected), ImGuiComboFlags_HeightLargest)) {
					for (u32 i = 0; i < lobby_data.size(); i++) {
						bool is_selected = i == lobby_selected;
						if (ImGui::Selectable(get_lobby_name(i), is_selected)) {
							lobby_selected = i;
							search_lobby_id = lobby_data[i][1];
							fetch_new_results();
						}
						if (is_selected) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				ImGui::PopItemWidth();

				break;
			}
			case 4:	 // No of Players
			{
				ImGui::SameLine();
				static int no_of_players_input = 2;
				ImGui::SliderInt("##no_of_players_input", &no_of_players_input, 2, 4);

				ImGui::SameLine();
				if (ImGui::Button("Add Filter")) {
					search_no_of_players = no_of_players_input;
					fetch_new_results();
				}

				break;
			}
			case 5:	 // Battle Code
			{
				static char battle_code_buf[100] = {0};
				ImGui::SameLine();
				if (ImGui::InputText("##battle_code_input", battle_code_buf, IM_ARRAYSIZE(battle_code_buf),
									 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter,
									 TextFilters::FilterNumber)) {
					search_battle_code = std::string(battle_code_buf);
					fetch_new_results();
				}

				ImGui::SameLine();
				if (ImGui::Button("Add Filter")) {
					search_battle_code = std::string(battle_code_buf);
					fetch_new_results();
				}

				break;
			}
			case 6:	 // Ranking
			{
				ImGui::SameLine();
				if (ImGui::Button("Yes")) {
					search_ranking = 1;
					fetch_new_results();
				}
				ImGui::SameLine();
				if (ImGui::Button("No")) {
					search_ranking = 0;
					fetch_new_results();
				}
				break;
			}
			case 7:	 // Disk
			{
				ImGui::SameLine();
				if (ImGui::Button("MUJI")) {
					search_disk = "dc1";
					fetch_new_results();
				}
				ImGui::SameLine();
				if (ImGui::Button("DX")) {
					search_disk = "dc2";
					fetch_new_results();
				}
				break;
			}
			case 8:	 // Used MS
			{
				static unsigned int ms_selected = 0;

				ImGui::SameLine();
				ImGui::PushItemWidth(200.0f * scaling);
				if (ImGui::BeginCombo("##UsedMSItems", ms_names(ms_selected), ImGuiComboFlags_HeightLargest)) {
					for (int i = 0; i < GdxsvLanguage::GetMSCount(); i++) {
						const char* name = ms_names(i);
						if (strlen(name) == 0) continue;
						bool is_selected = i == ms_selected;
						if (ImGui::Selectable(name, is_selected)) {
							ms_selected = i;
							search_used_ms = i + 1;  // 1-origin
							fetch_new_results();
						}
						if (is_selected) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				ImGui::PopItemWidth();

				break;
			}
			case 9:	 // Reverse
			{
				ImGui::SameLine();
				if (ImGui::Button("Add Filter")) {
					search_reverse = true;
					fetch_new_results();
				}
				break;
			}
			default:
				break;
		}

		ImGui::Dummy(ImVec2(0, 0));	 // Newline

		draw_filter_label_string("User ID", search_user_id);
		draw_filter_label_string("User Name", search_user_name);
		draw_filter_label_string("Pilot Name", search_pilot_name);
		draw_filter_label_string("Lobby ID", search_lobby_id);
		draw_filter_label_int("Players", search_no_of_players);
		draw_filter_label_string("Battle Code", search_battle_code);
		draw_filter_label_yesno("Ranking", search_ranking);
		if (!search_disk.empty()) {
			draw_filter_label("Disk", disk_display_name(search_disk), [&]() { search_disk = ""; });
		}
		draw_filter_label_bool("Reverse", search_reverse);
		if (search_used_ms != -1) {
			draw_filter_label("Used MS", std::to_string(search_used_ms), []() { search_used_ms = -1; });
		}
	}

	ImGui::BeginChild(ImGui::GetID("gdxsv_replay_server_list_paging"), ScaledVec2(450, 0), false, ImGuiWindowFlags_NoDecoration);
	{
		ImGui::BeginChild(ImGui::GetID("gdxsv_replay_server_list"), ImVec2(0, ImGui::GetContentRegionAvail().y - 40.f * scaling), true,
						  ImGuiWindowFlags_DragScrolling);
		{
			if (!fetch_replay_entry_future_.valid()) {
				fetch_replay_json();
			} else if (!future_is_ready(fetch_replay_entry_future_)) {
				ImGui::Text("Loading...");
			} else {
				const auto& entries = fetch_replay_entry_future_.get();
				if (entries.size() == 0 || fetch_replay_entry_http_status == 204) {
					ImGui::Text("No result");
				} else if (fetch_replay_entry_http_status != 200) {
					ImGui::Text("Error: HTTP %d", fetch_replay_entry_http_status);
				} else {
					const static auto item_spacing = 30.0f;
					ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ScaledVec2(0, item_spacing));
					for (int i = 0; i < entries.size(); ++i) {
						ImGui::PushID(i);
						const auto& entry = entries[i];

						time_t t = entry.start_unix;
						char timebuf[128] = {};
						std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));

						char head[256] = {};
						snprintf(head, sizeof(head), u8"  %s  %s ― Result: %d：%d\n\n", ICON_FA_FILM, timebuf, entry.renpo_win,
								 entry.zeon_win);
						std::string row = head;

						for (int i = 0; i < entry.users.size(); i++) {
							const auto& user = entry.users[i];
							row += user.user_name();
							if (i + 1 < entry.users.size()) {
								row += entry.users[i + 1].team() != user.team() ? " vs " : ", ";
							}
						}

						auto drawlist = ImGui::GetWindowDrawList();

						static auto SelectableColor = [drawlist](ImU32 color) {
							ImVec2 p_min = ImGui::GetItemRectMin();
							ImVec2 p_max = ImGui::GetItemRectMax();
							drawlist->AddRectFilled(p_min, p_max, color);
						};

						drawlist->ChannelsSplit(2);
						drawlist->ChannelsSetCurrent(1);

						if (ImGui::Selectable(row.c_str(), i == selected_replay_entry_index, 0, ImVec2(0, 0))) {
							selected_replay_entry_index = i;
							pov_index = -1;
						}
						if (i % 2 == 1) {
							drawlist->ChannelsSetCurrent(0);
							SelectableColor(IM_COL32(50, 50, 50, 100));
						}
						drawlist->ChannelsMerge();

						ImGui::PopID();
					}
					ImGui::PopStyleVar();
				}
			}
		}
		scrollWhenDraggingOnVoid();
		windowDragScroll();
		ImGui::EndChild();

		{
			DisabledScope loading_scope(!future_is_ready(fetch_replay_entry_future_));
			{
				{
					DisabledScope scope(entry_paging == 0);
					if (ImGui::Button(ICON_FA_CHEVRON_LEFT "  Prev Page") && !scope.isDisabled()) {
						entry_paging--;
						snprintf(page_buf, sizeof(page_buf), "%d", entry_paging + 1);
						fetch_target_page();
					}
				}
				{
					ImGui::SameLine();
					ImGui::SetNextItemWidth(60.f * scaling);
					if (ImGui::InputText("##page_input", page_buf, IM_ARRAYSIZE(page_buf),
						ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter,
						TextFilters::FilterNumber)) {
						entry_paging = atoi(page_buf) - 1;
						if (entry_paging < 0) entry_paging = 0;
						fetch_target_page();
					}
					ImGui::SameLine();
					DisabledScope scope(future_is_ready(fetch_replay_entry_future_) && fetch_replay_entry_future_.get().size() < 100);
					if (ImGui::Button(ICON_FA_CHEVRON_RIGHT "  Next Page")) {
						entry_paging++;
						snprintf(page_buf, sizeof(page_buf), "%d", entry_paging + 1);
						fetch_target_page();
					}
				}
			}
		}
	}
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginChild(ImGui::GetID("gdxsv_replay_server_detail"), ImVec2(0, 0), true, ImGuiWindowFlags_DragScrolling);
	{
		if (selected_replay_entry_index != -1) {
			const auto& entries = fetch_replay_entry_future_.get();
			const auto& entry = entries[selected_replay_entry_index];
			std::string battle_code = entry.replay_url.substr(entry.replay_url.find_last_of("/") + 1);
			battle_code = battle_code.substr(0, battle_code.find(".pb"));

			gdxsv_replay_draw_info(battle_code, entry.disk, (int)entry.users.size(), "", entry.start_unix, 0, entry.users,
								   entry.replay_url, entry.play_count);
			draw_round_detail(entry);
		}
	}
	scrollWhenDraggingOnVoid();
	windowDragScroll();
	ImGui::EndChild();
}

}  // namespace

bool gdxsv_ensure_replay_savestate(int disk) {
	const auto save_path = hostfs::getSavestatePath(99, false);
	if (file_exists(save_path)) {
		return true;
	}

	http::init();
	std::vector<u8> downloaded;
	std::string content_type;
	const std::string url = disk == 1 ? "https://storage.googleapis.com/gdxsv/misc/gdx-disc1_99.state"
									  : "https://storage.googleapis.com/gdxsv/misc/gdx-disc2_99.state";
	int rc = http::get(url, downloaded, content_type);
	if (rc != 200) {
		ERROR_LOG(COMMON, "replay savestate download failure rc=%d url=%s", rc, url.c_str());
		return false;
	}

	FILE* fp = nowide::fopen(save_path.c_str(), "wb");
	if (fp == nullptr) {
		ERROR_LOG(COMMON, "replay savestate save failure: %s", save_path.c_str());
		return false;
	}

	const auto written = fwrite(downloaded.data(), 1, downloaded.size(), fp);
	std::fclose(fp);
	return written == downloaded.size();
}

int gdxsv_live_viewer_count(const std::string& battle_code, bool force_refresh) {
	if (battle_code.empty()) return 0;

	if (fetch_viewers_future_.valid() && future_is_ready(fetch_viewers_future_)) {
		live_viewer_count_ = fetch_viewers_future_.get();
		fetch_viewers_future_ = std::shared_future<int>();
	}

	const auto now = std::chrono::steady_clock::now();
	const bool due = kViewerRefreshSeconds <= std::chrono::duration_cast<std::chrono::seconds>(now - viewers_last_fetch_).count();
	if (!fetch_viewers_future_.valid() && (due || force_refresh)) {
		viewers_last_fetch_ = now;
		// Capture the current count rather than reading the global from the
		// worker: the main thread writes it when a fetch lands.
		const int last_known = live_viewer_count_;
		fetch_viewers_future_ = std::async(std::launch::async, [battle_code, last_known]() -> int {
			std::vector<u8> dl;
			std::string content_type;
			http::init();
			const std::string url =
				config::loadStr("gdxsv", "LiveApiUrl", "https://asia-northeast1-gdxsv-274515.cloudfunctions.net/lbsapi") +
				"/spectators?battle_code=" + http::urlEncode(battle_code);
			if (http::get(url, dl, content_type) != 200) return last_known;
			try {
				nlohmann::json j = nlohmann::json::parse(dl);
				return j.value("spectators", 0);
			} catch (const nlohmann::json::exception&) {
			}
			return last_known;
		}).share();
	}

	return live_viewer_count_;
}

void gdxsv_start_live_spectate(const std::string& battle_code, int pov) {
	if (gdxsv.IsSaveStateAllowed()) {
		dc_savestate(90);
	}

	if (gdxsv_ensure_replay_savestate(gdxsv.Disk())) {
		dc_loadstate(99);
		if (gdxsv.StartLiveSpectate(battle_code.c_str(), pov)) {
			gui_state = GuiState::Closed;
		}
	}
}

void gdxsv_start_replay(const std::string& replay_file, int pov) {
	if (gdxsv.IsSaveStateAllowed()) {
		dc_savestate(90);
	}

	if (gdxsv_ensure_replay_savestate(gdxsv.Disk())) {
		dc_loadstate(99);
		if (gdxsv.StartReplayFile(replay_file.c_str(), pov)) {
			gui_state = GuiState::Closed;
			// Fire-and-forget: notify server of replay play (HTTP replays only)
			if (replay_file.find("http") == 0) {
				auto pos = replay_file.find_last_of("/");
				if (pos != std::string::npos) {
					std::string battle_code = replay_file.substr(pos + 1);
					auto dot = battle_code.find(".pb");
					if (dot != std::string::npos) {
						battle_code = battle_code.substr(0, dot);
					}
					std::thread([battle_code]() {
						http::init();
						std::vector<u8> dl;
						std::string content_type;
						auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
							std::chrono::system_clock::now().time_since_epoch()).count();
						std::string url = "https://asia-northeast1-gdxsv-274515.cloudfunctions.net/lbsapi/replay_played?battle_code="
							+ http::urlEncode(battle_code) + "&_t=" + std::to_string(ts);
						http::get(url, dl, content_type);
					}).detach();
				}
			}
		} else {
			dc_loadstate(90);
			broken_replay_path = replay_file;
		}
	}
}

void gdxsv_end_replay() {
	emu.stop();
	dc_loadstate(90);
	settings.input.fastForwardMode = false;

	if (!selected_replay_file.empty() || selected_replay_entry_index != -1) {
		gui_state = GuiState::GdxsvReplay;
	} else {
		// Replay from command-line, resume game when end replaying
		emu.start();
	}
}

void gdxsv_replay_select_dialog() {
	centerNextWindow();
	ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);

	scaling = settings.display.uiScale;
	normal_padding = ImGui::GetStyle().FramePadding;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::Begin("Live & Replays##gdxsv_emu_replay_menu", nullptr,
				 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

	if (ImGui::Button(ICON_FA_XMARK  "  Close", ScaledVec2(100, 40))) {
		gui_state = GuiState::Commands;
	}

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(16, 6));
	if (ImGui::BeginTabBar("replays", ImGuiTabBarFlags_NoTooltip)) {
		if (ImGui::BeginTabItem(ICON_FA_TOWER_BROADCAST "  Live")) {
			gdxsv_replay_live_tab();
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem(ICON_FA_FOLDER "  Local")) {
			gdxsv_replay_local_tab();
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem(ICON_FA_GLOBE "  Server")) {
			gdxsv_replay_server_tab();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	ImGui::PopStyleVar();

	ImGui::End();
	ImGui::PopStyleVar();  // ImGuiStyleVar_WindowRounding
}
