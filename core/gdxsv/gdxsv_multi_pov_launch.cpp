// Host and guest orchestration for 4-player replay: who reads the replay, who
// is spawned with what, and the one barrier they all line up at before the
// first frame. The transport itself is in gdxsv_multi_pov.cpp.
#include <algorithm>
#include <iterator>
#include <cstdio>
#include <string>
#include <vector>

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "gdxsv.pb.h"
#include "gdxsv_multi_pov.h"
#include "log/LogManager.h"
#include <nowide/cstdio.hpp>

#include "oslib/http_client.h"
#include "oslib/oslib.h"
#include "stdclass.h"
#include "types.h"

// A guest cold-boots the game and loads a savestate while the host is already
// sitting in the replay browser, so the host has to be prepared to wait out a
// whole startup. Generous on purpose: the cost of waiting too long is a slow
// start, the cost of waiting too little is a screen that misses the barrier
// and plays on its own.
constexpr int kStartBarrierMs = 180 * 1000;

// The host's own wait at the barrier. Short, because the UI has already held
// the start until the guests reported in (see gdxsv_start_replay): this only
// covers the last of them arriving between that check and the first frame.
constexpr int kHostBarrierSettleMs = 5 * 1000;

// How long a guest waits for the host to publish the payload. The host writes
// it before spawning anyone, so this only has to cover the mapping itself.
constexpr int kReplayFetchMs = 30 * 1000;

// Config keys the host passes to a guest on the command line. Transient (see
// setTransient in cfg/cl.cpp), so a guest never writes them to the config
// file and an ordinary later run is unaffected.
constexpr char kSessionKey[] = "MultiPovSession";
constexpr char kScreenKey[] = "MultiPovScreen";

// Set once by the host when it spawns: how many guests the barrier should
// expect. Only meaningful in the host process.
static int g_spawned_guests = 0;

static std::string SessionId() { return config::loadStr("gdxsv", kSessionKey, ""); }

static int ScreenArg() { return config::loadInt("gdxsv", kScreenKey, 0); }

static bool ReadLocalFile(const std::string& path, std::vector<uint8_t>& out) {
	FILE* fp = nowide::fopen(path.c_str(), "rb");
	if (fp == nullptr) {
		WARN_LOG(COMMON, "multi-pov: cannot open replay %s", path.c_str());
		return false;
	}
	std::fseek(fp, 0, SEEK_END);
	const long size = std::ftell(fp);
	std::fseek(fp, 0, SEEK_SET);
	if (size <= 0) {
		std::fclose(fp);
		WARN_LOG(COMMON, "multi-pov: empty replay %s", path.c_str());
		return false;
	}
	out.resize(static_cast<size_t>(size));
	const size_t read = std::fread(out.data(), 1, out.size(), fp);
	std::fclose(fp);
	if (read != out.size()) {
		WARN_LOG(COMMON, "multi-pov: short read on replay %s", path.c_str());
		return false;
	}
	return true;
}

// The host is the only process that ever touches the replay source, local or
// remote. Everything else is handed the bytes.
static bool LoadReplaySource(const std::string& source, std::vector<uint8_t>& out) {
	if (source.compare(0, 4, "http") == 0) {
		http::init();
		std::string content_type;
		std::vector<u8> downloaded;
		const int rc = http::get(source, downloaded, content_type);
		if (rc != 200) {
			WARN_LOG(COMMON, "multi-pov: replay download failed rc=%d %s", rc, source.c_str());
			return false;
		}
		out.assign(downloaded.begin(), downloaded.end());
		return true;
	}
	return ReadLocalFile(source, out);
}

// Four screens only make sense for a four-player battle; anything else falls
// back to ordinary single-screen playback rather than opening blank windows.
static bool IsFourPlayerBattle(const std::vector<uint8_t>& replay) {
	proto::BattleLogFile log;
	if (!log.ParseFromArray(replay.data(), static_cast<int>(replay.size()))) {
		WARN_LOG(COMMON, "multi-pov: replay does not parse");
		return false;
	}
	if (log.users_size() != kGdxsvMultiPovScreens) {
		NOTICE_LOG(COMMON, "multi-pov: battle has %d players, not %d - using one screen", log.users_size(), kGdxsvMultiPovScreens);
		return false;
	}
	return true;
}

// Where the grid goes when it starts. Flycast only writes the window geometry
// out when it closes, so this is the last known placement rather than the
// live one; the window layer repositions the grid from the host's real rect
// once it is up.
static GdxsvMultiPovRect InitialGroupRect() {
	GdxsvMultiPovRect group;
	group.w = std::max(320, config::loadInt("window", "width", 1280)) * 2;
	group.h = std::max(240, config::loadInt("window", "height", 720)) * 2;
	group.x = config::loadInt("window", "left", 0);
	group.y = config::loadInt("window", "top", 0);
	return group;
}

static void SpawnGuest(int screen, const std::string& session_id, const GdxsvMultiPovRect& quadrant) {
	const std::string session = std::string("gdxsv:") + kSessionKey + "=" + session_id;
	const std::string screen_arg = std::string("gdxsv:") + kScreenKey + "=" + std::to_string(screen);
	// The existing per-frame group barrier (GdxsvSpectateSync) keeps the four
	// on the same frame once they are running; the session id names the group.
	const std::string sync_group = "gdxsv:SpectateSyncGroup=" + session_id;
	const std::string pov = "gdxsv:ReplayPOV=" + std::to_string(screen + 1);
	const std::string left = "window:left=" + std::to_string(quadrant.x);
	const std::string top = "window:top=" + std::to_string(quadrant.y);
	const std::string width = "window:width=" + std::to_string(quadrant.w);
	const std::string height = "window:height=" + std::to_string(quadrant.h);
	// A guest is a quadrant, whatever the user's own window was doing last
	// time. Without these it would come up maximized or full screen and sit on
	// top of the grid.
	const std::string maximized = "window:maximized=no";
	const std::string fullscreen = "window:fullscreen=no";
	// Four instances mixing the same battle is noise, not four soundtracks:
	// only the host - the screen the user is actually driving - is audible.
	// The volume rather than settings.aica.muteAudio because that flag belongs
	// to the replay backend, which toggles it around seeks and fast-forward, so
	// a guest could not hold it down. Transient like the rest, so the user's own
	// volume setting is untouched.
	const std::string volume = "config:aica.Volume=0";
	const std::string content = settings.content.path;

	const char* args[] = {
		"-config", session.c_str(),
		"-config", screen_arg.c_str(),
		"-config", sync_group.c_str(),
		"-config", pov.c_str(),
		"-config", left.c_str(),
		"-config", top.c_str(),
		"-config", width.c_str(),
		"-config", height.c_str(),
		"-config", maximized.c_str(),
		"-config", fullscreen.c_str(),
		"-config", volume.c_str(),
		content.c_str(),
	};
	NOTICE_LOG(COMMON, "multi-pov: spawning %dP at %d,%d %dx%d", screen + 1, quadrant.x, quadrant.y, quadrant.w, quadrant.h);
	os_RunInstance(static_cast<int>(std::size(args)), args);
}

void gdxsv_multi_pov_compute_grid(const GdxsvMultiPovRect& group, GdxsvMultiPovRect out[kGdxsvMultiPovScreens]) {
	// Halves, with the remainder going to the right and bottom cells so the
	// four add back up to the group exactly.
	const int32_t left_w = group.w / 2;
	const int32_t top_h = group.h / 2;
	const int32_t right_w = group.w - left_w;
	const int32_t bottom_h = group.h - top_h;

	out[0] = {group.x, group.y, left_w, top_h};
	out[1] = {group.x + left_w, group.y, right_w, top_h};
	out[2] = {group.x, group.y + top_h, left_w, bottom_h};
	out[3] = {group.x + left_w, group.y + top_h, right_w, bottom_h};
}

bool gdxsv_multi_pov_four_screen_requested() {
	// A guest is told what it is; it must never read the checkbox and try to
	// start a session of its own.
	if (0 < ScreenArg()) return false;
	return config::GdxReplayFourScreen.get();
}

int gdxsv_multi_pov_spawned_guest_count() { return g_spawned_guests; }

int gdxsv_multi_pov_guest_pov() {
	const int screen = ScreenArg();
	if (screen < 1 || kGdxsvMultiPovScreens <= screen) return -1;
	return screen;
}

bool gdxsv_multi_pov_begin_host_session(const std::string& replay_source, std::vector<uint8_t>& replay_out) {
	g_spawned_guests = 0;

	if (!LoadReplaySource(replay_source, replay_out)) return false;
	if (!IsFourPlayerBattle(replay_out)) return false;

	const std::string session_id = gdxsv_multi_pov_new_session_id();
	if (!gdxsv_multi_pov_host_create(session_id, replay_out)) return false;

	// The host plays 1P and joins the same frame-sync group it puts the
	// guests in, so all four are held together once they are running.
	config::setTransient("gdxsv", "SpectateSyncGroup", session_id);

	GdxsvMultiPovRect quadrants[kGdxsvMultiPovScreens];
	gdxsv_multi_pov_compute_grid(InitialGroupRect(), quadrants);

	for (int screen = 1; screen < kGdxsvMultiPovScreens; ++screen) {
		SpawnGuest(screen, session_id, quadrants[screen]);
		++g_spawned_guests;
	}
	NOTICE_LOG(COMMON, "multi-pov: host session %s with %d guests", session_id.c_str(), g_spawned_guests);
	return true;
}

bool gdxsv_multi_pov_begin_guest_session(std::vector<uint8_t>& replay_out) {
	const int screen = gdxsv_multi_pov_guest_pov();
	if (screen < 0) return false;

	if (!gdxsv_multi_pov_guest_open(SessionId(), screen)) return false;
	if (!gdxsv_multi_pov_fetch_replay(replay_out, kReplayFetchMs)) {
		gdxsv_multi_pov_close();
		return false;
	}
	return true;
}

void gdxsv_multi_pov_wait_at_start_barrier() {
	switch (gdxsv_multi_pov_current_role()) {
		case GdxsvMultiPovRole::Host:
			gdxsv_multi_pov_host_wait_for_guests(g_spawned_guests, kHostBarrierSettleMs);
			break;
		case GdxsvMultiPovRole::Guest:
			gdxsv_multi_pov_guest_ready_and_wait(kStartBarrierMs);
			break;
		case GdxsvMultiPovRole::None:
			break;
	}
}

