#include "gdxsv_multi_pov.h"

#ifdef _WIN32
// MinGW's libstdc++ defines this already, so guard it.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "log/LogManager.h"
#include "types.h"

constexpr uint32_t kMagic = 0x4D505634;	 // "MPV4"
constexpr uint32_t kVersion = 3;

// The replay payload follows the header at this offset.
constexpr size_t kPayloadOffset = 4096;

// Sanity cap on the replay size read from a header another process wrote.
constexpr uint64_t kMaxReplayBytes = 256ull * 1024 * 1024;

// "Gone" means the process is gone: a heartbeat cannot tell a busy host (one
// loading a game holds its UI thread for seconds) from a dead one.
static bool ProcessAlive(int32_t pid) {
	if (pid <= 0) return false;
#ifdef _WIN32
	HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
	if (proc == nullptr) return false;
	const bool alive = WaitForSingleObject(proc, 0) == WAIT_TIMEOUT;
	CloseHandle(proc);
	return alive;
#else
	// EPERM means it exists and is not ours; ESRCH means it is gone.
	return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

static int32_t CurrentPid() {
#ifdef _WIN32
	return static_cast<int32_t>(GetCurrentProcessId());
#else
	return static_cast<int32_t>(getpid());
#endif
}

// Shared by the four processes. Every field has a single writer, so atomics
// are enough.
struct GdxsvMultiPovHeader {
	std::atomic<uint32_t> magic;
	std::atomic<uint32_t> version;
	std::atomic<uint64_t> total_size;
	std::atomic<uint64_t> replay_size;
	std::atomic<uint32_t> replay_ready;

	std::atomic<int32_t> host_pid;
	std::atomic<uint32_t> host_closed;

	// Start barrier: each guest sets its bit, the host raises `go`.
	std::atomic<uint32_t> ready_mask;
	std::atomic<uint32_t> go;

	std::atomic<int32_t> guest_pid[kGdxsvMultiPovScreens];

	// Playback, written by the host every frame.
	std::atomic<int64_t> position;
	std::atomic<int32_t> speed;
	std::atomic<uint32_t> paused;
	std::atomic<uint32_t> menu_open;
	std::atomic<uint32_t> seek_generation;
	std::atomic<int64_t> seek_target;
	std::atomic<int32_t> seek_round;
	std::atomic<uint32_t> show_ally_hp;
	std::atomic<uint32_t> key_display;
	std::atomic<uint32_t> skip_ms_selection;
	std::atomic<int32_t> volume;
	// The guests start before the host publishes; until then the fields
	// above are zero and must not be followed.
	std::atomic<uint32_t> playback_published;

	// Window layout, written by the host whenever it moves or resizes.
	std::atomic<int32_t> win_x, win_y, win_w, win_h;
	std::atomic<int32_t> grp_x, grp_y, grp_w, grp_h;
	std::atomic<uint32_t> maximized;
	std::atomic<uint32_t> fullscreen;
	std::atomic<uint32_t> win_generation;
};

static_assert(sizeof(GdxsvMultiPovHeader) <= kPayloadOffset, "header must fit before the payload");

static std::string SessionPath(const std::string& session_id) {
#ifdef _WIN32
	char temp_dir[MAX_PATH];
	if (GetTempPathA(MAX_PATH, temp_dir) == 0) return {};
	return std::string(temp_dir) + "gdxsv_multi_pov_" + session_id;
#else
	return "/tmp/gdxsv_multi_pov_" + session_id;
#endif
}

// Maps the session file. `create` lays it out at `size`; otherwise it must
// already be at least that big (reading past the end is a SIGBUS on POSIX).
static void* MapSession(const std::string& path, size_t size, bool create) {
#ifdef _WIN32
	HANDLE file = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
							  create ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		WARN_LOG(COMMON, "multi-pov: CreateFile failed %s (%lu)", path.c_str(), GetLastError());
		return nullptr;
	}
	if (!create) {
		LARGE_INTEGER on_disk{};
		if (!GetFileSizeEx(file, &on_disk) || static_cast<uint64_t>(on_disk.QuadPart) < size) {
			CloseHandle(file);
			return nullptr;
		}
	}
	HANDLE mapping = CreateFileMappingA(file, nullptr, PAGE_READWRITE, static_cast<DWORD>(static_cast<uint64_t>(size) >> 32),
										static_cast<DWORD>(size & 0xffffffffu), nullptr);
	if (mapping == nullptr) {
		WARN_LOG(COMMON, "multi-pov: CreateFileMapping failed %s (%lu)", path.c_str(), GetLastError());
		CloseHandle(file);
		return nullptr;
	}
	void* m = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
	CloseHandle(mapping);  // the view keeps the section alive
	CloseHandle(file);
	if (m == nullptr) WARN_LOG(COMMON, "multi-pov: MapViewOfFile failed (%lu)", GetLastError());
	return m;
#else
	const int fd = open(path.c_str(), create ? (O_RDWR | O_CREAT | O_TRUNC) : O_RDWR, 0666);
	if (fd < 0) {
		WARN_LOG(COMMON, "multi-pov: open failed %s", path.c_str());
		return nullptr;
	}
	if (create) {
		if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
			WARN_LOG(COMMON, "multi-pov: ftruncate failed %s", path.c_str());
			close(fd);
			return nullptr;
		}
	} else {
		struct stat st {};
		if (fstat(fd, &st) != 0 || static_cast<uint64_t>(st.st_size) < size) {
			close(fd);
			return nullptr;
		}
	}
	void* m = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED) {
		WARN_LOG(COMMON, "multi-pov: mmap failed %s", path.c_str());
		return nullptr;
	}
	return m;
#endif
}

static void UnmapSession(void* m, size_t size) {
	if (m == nullptr) return;
#ifdef _WIN32
	(void)size;
	UnmapViewOfFile(m);
#else
	munmap(m, size);
#endif
}

static uint64_t SessionFileSize(const std::string& path) {
#ifdef _WIN32
	WIN32_FILE_ATTRIBUTE_DATA attr{};
	if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attr)) return 0;
	return (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
#else
	struct stat st {};
	if (stat(path.c_str(), &st) != 0) return 0;
	return static_cast<uint64_t>(st.st_size);
#endif
}

// This process's session: at most one per process.
struct GdxsvMultiPovSession {
	GdxsvMultiPovRole role = GdxsvMultiPovRole::None;
	int screen = -1;
	void* map = nullptr;
	size_t map_size = 0;
	std::string path;

	GdxsvMultiPovHeader* header() const { return static_cast<GdxsvMultiPovHeader*>(map); }
	uint8_t* payload() const { return static_cast<uint8_t*>(map) + kPayloadOffset; }
};

static GdxsvMultiPovSession g_session;

static bool HostAlive(const GdxsvMultiPovHeader* h) {
	if (h->host_closed.load(std::memory_order_acquire) != 0) return false;
	const int32_t pid = h->host_pid.load(std::memory_order_acquire);
	return pid == 0 || ProcessAlive(pid);  // 0: still being created
}

std::string gdxsv_multi_pov_new_session_id() {
	const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	return std::to_string(CurrentPid()) + "_" + std::to_string(now);
}

bool gdxsv_multi_pov_host_create(const std::string& session_id, const std::vector<uint8_t>& replay) {
	if (session_id.empty()) return false;
	if (replay.empty() || kMaxReplayBytes < replay.size()) {
		WARN_LOG(COMMON, "multi-pov: refusing to publish a %zu byte replay", replay.size());
		return false;
	}
	gdxsv_multi_pov_close();

	const std::string path = SessionPath(session_id);
	if (path.empty()) return false;

	const size_t size = kPayloadOffset + replay.size();
	void* m = MapSession(path, size, true);
	if (m == nullptr) return false;

	g_session.role = GdxsvMultiPovRole::Host;
	g_session.screen = 0;
	g_session.map = m;
	g_session.map_size = size;
	g_session.path = path;

	GdxsvMultiPovHeader* h = g_session.header();
	std::memset(m, 0, kPayloadOffset);
	h->version.store(kVersion, std::memory_order_relaxed);
	h->total_size.store(size, std::memory_order_relaxed);
	h->host_pid.store(CurrentPid(), std::memory_order_relaxed);

	std::memcpy(g_session.payload(), replay.data(), replay.size());
	h->replay_size.store(replay.size(), std::memory_order_relaxed);
	h->replay_ready.store(1, std::memory_order_release);
	h->magic.store(kMagic, std::memory_order_release);

	NOTICE_LOG(COMMON, "multi-pov: host published %zu replay bytes to session %s", replay.size(), session_id.c_str());
	return true;
}

bool gdxsv_multi_pov_guest_open(const std::string& session_id, int screen) {
	if (session_id.empty()) return false;
	if (screen < 1 || kGdxsvMultiPovScreens <= screen) {
		WARN_LOG(COMMON, "multi-pov: bad guest screen index %d", screen);
		return false;
	}
	gdxsv_multi_pov_close();

	const std::string path = SessionPath(session_id);
	if (path.empty()) return false;

	// The host lays the whole file out before spawning anyone.
	const uint64_t on_disk = SessionFileSize(path);
	if (on_disk < kPayloadOffset) {
		WARN_LOG(COMMON, "multi-pov: session %s is not ready (%llu bytes)", session_id.c_str(), (unsigned long long)on_disk);
		return false;
	}

	void* m = MapSession(path, static_cast<size_t>(on_disk), false);
	if (m == nullptr) return false;

	GdxsvMultiPovHeader* h = static_cast<GdxsvMultiPovHeader*>(m);
	if (h->magic.load(std::memory_order_acquire) != kMagic || h->version.load(std::memory_order_acquire) != kVersion) {
		WARN_LOG(COMMON, "multi-pov: session %s has a bad header", session_id.c_str());
		UnmapSession(m, static_cast<size_t>(on_disk));
		return false;
	}

	g_session.role = GdxsvMultiPovRole::Guest;
	g_session.screen = screen;
	g_session.map = m;
	g_session.map_size = static_cast<size_t>(on_disk);
	g_session.path = path;
	g_session.header()->guest_pid[screen].store(CurrentPid(), std::memory_order_release);

	NOTICE_LOG(COMMON, "multi-pov: guest %dP joined session %s", screen + 1, session_id.c_str());
	return true;
}

void gdxsv_multi_pov_close() {
	if (g_session.map == nullptr) {
		g_session.role = GdxsvMultiPovRole::None;
		g_session.screen = -1;
		return;
	}
	GdxsvMultiPovHeader* h = g_session.header();
	if (g_session.role == GdxsvMultiPovRole::Host) {
		h->host_closed.store(1, std::memory_order_release);
	} else if (0 <= g_session.screen && g_session.screen < kGdxsvMultiPovScreens) {
		h->guest_pid[g_session.screen].store(0, std::memory_order_release);
	}
	UnmapSession(g_session.map, g_session.map_size);
	g_session.map = nullptr;
	g_session.map_size = 0;
	g_session.role = GdxsvMultiPovRole::None;
	g_session.screen = -1;
	g_session.path.clear();
}

GdxsvMultiPovRole gdxsv_multi_pov_current_role() { return g_session.role; }

int gdxsv_multi_pov_screen_index() { return g_session.screen; }

bool gdxsv_multi_pov_fetch_replay(std::vector<uint8_t>& out, int timeout_ms) {
	GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr) return false;

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (h->replay_ready.load(std::memory_order_acquire) == 0) {
		if (!HostAlive(h)) {
			WARN_LOG(COMMON, "multi-pov: host went away before publishing the replay");
			return false;
		}
		if (deadline <= std::chrono::steady_clock::now()) {
			WARN_LOG(COMMON, "multi-pov: timed out waiting for the replay payload");
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	const uint64_t size = h->replay_size.load(std::memory_order_acquire);
	if (size == 0 || kPayloadOffset + size > g_session.map_size) {
		WARN_LOG(COMMON, "multi-pov: replay size %llu does not fit the session", (unsigned long long)size);
		return false;
	}
	out.assign(g_session.payload(), g_session.payload() + size);
	return true;
}

bool gdxsv_multi_pov_guest_ready_and_wait(int timeout_ms) {
	GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr || g_session.role != GdxsvMultiPovRole::Guest) return false;

	h->ready_mask.fetch_or(1u << g_session.screen, std::memory_order_acq_rel);

	const auto waiting_since = std::chrono::steady_clock::now();
	const auto deadline = waiting_since + std::chrono::milliseconds(timeout_ms);
	while (h->go.load(std::memory_order_acquire) == 0) {
		if (!HostAlive(h)) {
			WARN_LOG(COMMON, "multi-pov: host went away before the start signal");
			return false;
		}
		if (deadline <= std::chrono::steady_clock::now()) {
			WARN_LOG(COMMON, "multi-pov: no start signal within %d ms; starting anyway", timeout_ms);
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	NOTICE_LOG(COMMON, "multi-pov: start barrier released after %lld ms",
			   (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waiting_since).count());
	return true;
}

static int ReadyGuestCount() {
	const GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr) return 0;
	const uint32_t mask = h->ready_mask.load(std::memory_order_acquire);
	int n = 0;
	for (int i = 1; i < kGdxsvMultiPovScreens; ++i)
		if (mask & (1u << i)) ++n;
	return n;
}

bool gdxsv_multi_pov_host_wait_for_guests(int expected_guests, int timeout_ms) {
	GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr || g_session.role != GdxsvMultiPovRole::Host) return false;
	if (expected_guests <= 0) {
		h->go.store(1, std::memory_order_release);
		return true;
	}

	const auto waiting_since = std::chrono::steady_clock::now();
	const auto deadline = waiting_since + std::chrono::milliseconds(timeout_ms);
	bool all_in = false;
	while (std::chrono::steady_clock::now() < deadline) {
		if (expected_guests <= ReadyGuestCount()) {
			all_in = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	if (!all_in)
		WARN_LOG(COMMON, "multi-pov: only %d of %d guests reported ready; starting without the rest", ReadyGuestCount(), expected_guests);

	h->go.store(1, std::memory_order_release);
	NOTICE_LOG(COMMON, "multi-pov: start barrier released %d screens after %lld ms", ReadyGuestCount() + 1,
			   (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waiting_since).count());
	return all_in;
}

void gdxsv_multi_pov_publish_playback(const GdxsvMultiPovPlayback& state) {
	GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr || g_session.role != GdxsvMultiPovRole::Host) return;
	h->position.store(state.position, std::memory_order_relaxed);
	h->speed.store(state.speed, std::memory_order_relaxed);
	h->paused.store(state.paused ? 1u : 0u, std::memory_order_relaxed);
	h->menu_open.store(state.menu_open ? 1u : 0u, std::memory_order_relaxed);
	h->seek_target.store(state.seek_target, std::memory_order_relaxed);
	h->seek_round.store(state.seek_round, std::memory_order_relaxed);
	h->show_ally_hp.store(state.show_ally_hp ? 1u : 0u, std::memory_order_relaxed);
	h->key_display.store(state.key_display ? 1u : 0u, std::memory_order_relaxed);
	h->skip_ms_selection.store(state.skip_ms_selection ? 1u : 0u, std::memory_order_relaxed);
	h->volume.store(state.volume, std::memory_order_relaxed);
	// Last, so a guest that sees the generation sees the target with it.
	h->seek_generation.store(state.seek_generation, std::memory_order_release);
	h->playback_published.store(1, std::memory_order_release);
}

bool gdxsv_multi_pov_read_playback(GdxsvMultiPovPlayback& out) {
	const GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr) return false;
	if (h->playback_published.load(std::memory_order_acquire) == 0) return false;
	out.seek_generation = h->seek_generation.load(std::memory_order_acquire);
	out.seek_target = h->seek_target.load(std::memory_order_relaxed);
	out.seek_round = h->seek_round.load(std::memory_order_relaxed);
	out.position = h->position.load(std::memory_order_relaxed);
	out.speed = h->speed.load(std::memory_order_relaxed);
	out.paused = h->paused.load(std::memory_order_relaxed) != 0;
	out.menu_open = h->menu_open.load(std::memory_order_relaxed) != 0;
	out.show_ally_hp = h->show_ally_hp.load(std::memory_order_relaxed) != 0;
	out.key_display = h->key_display.load(std::memory_order_relaxed) != 0;
	out.skip_ms_selection = h->skip_ms_selection.load(std::memory_order_relaxed) != 0;
	out.volume = h->volume.load(std::memory_order_relaxed);
	return true;
}

void gdxsv_multi_pov_publish_host_window(const GdxsvMultiPovHostWindow& window) {
	GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr || g_session.role != GdxsvMultiPovRole::Host) return;
	h->win_x.store(window.rect.x, std::memory_order_relaxed);
	h->win_y.store(window.rect.y, std::memory_order_relaxed);
	h->win_w.store(window.rect.w, std::memory_order_relaxed);
	h->win_h.store(window.rect.h, std::memory_order_relaxed);
	h->grp_x.store(window.group.x, std::memory_order_relaxed);
	h->grp_y.store(window.group.y, std::memory_order_relaxed);
	h->grp_w.store(window.group.w, std::memory_order_relaxed);
	h->grp_h.store(window.group.h, std::memory_order_relaxed);
	h->maximized.store(window.maximized ? 1u : 0u, std::memory_order_relaxed);
	h->fullscreen.store(window.fullscreen ? 1u : 0u, std::memory_order_relaxed);
	h->win_generation.store(window.generation, std::memory_order_release);
}

bool gdxsv_multi_pov_read_host_window(GdxsvMultiPovHostWindow& out) {
	const GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr) return false;
	out.generation = h->win_generation.load(std::memory_order_acquire);
	if (out.generation == 0) return false;
	out.rect.x = h->win_x.load(std::memory_order_relaxed);
	out.rect.y = h->win_y.load(std::memory_order_relaxed);
	out.rect.w = h->win_w.load(std::memory_order_relaxed);
	out.rect.h = h->win_h.load(std::memory_order_relaxed);
	out.group.x = h->grp_x.load(std::memory_order_relaxed);
	out.group.y = h->grp_y.load(std::memory_order_relaxed);
	out.group.w = h->grp_w.load(std::memory_order_relaxed);
	out.group.h = h->grp_h.load(std::memory_order_relaxed);
	out.maximized = h->maximized.load(std::memory_order_relaxed) != 0;
	out.fullscreen = h->fullscreen.load(std::memory_order_relaxed) != 0;
	return true;
}

bool gdxsv_multi_pov_host_gone() {
	const GdxsvMultiPovHeader* h = g_session.header();
	if (h == nullptr) return false;
	return !HostAlive(h);
}

