#pragma once
#include <cstdint>
#include <string>
#include <vector>

// "4-player replay": one replay watched from all four players' points of view
// at once, as four Flycast processes laid out in a 2x2 grid.
//
//     [1P, 2P]
//     [3P, 4P]
//
// The 1P instance is the HOST: it is the one the user started, it is the only
// one that reads the replay, and it drives playback and the window layout. The
// other three are GUESTS, spawned by the host with os_RunInstance. A guest is
// fed everything it needs - the replay bytes included - through the session
// below, so it never touches the network or the replay store.
//
// Transport: a file-backed shared memory mapping, the same technique
// GdxsvSpectateSync uses (see gdxsv_spectate_sync.cpp for why a real file and
// not an anonymous "Local\\" section: that namespace is scoped to the creating
// process's session, and child processes can land in a different one).
// Shared memory rather than a socket because the payload is a whole replay of
// a few MB that every guest reads once - a mapping hands that over with no
// framing protocol, no port, and no firewall prompt - and because the control
// block is a handful of scalars published every frame, which is exactly what a
// single-writer atomic is good at.

// 2x2: 1P top-left, 2P top-right, 3P bottom-left, 4P bottom-right.
constexpr int kGdxsvMultiPovScreens = 4;

enum class GdxsvMultiPovRole {
	None,	// ordinary single-screen playback
	Host,	// 1P: reads the replay, spawns and drives the guests
	Guest,	// 2P-4P: fed by the host
};

struct GdxsvMultiPovRect {
	int32_t x = 0;
	int32_t y = 0;
	int32_t w = 0;
	int32_t h = 0;

	bool operator==(const GdxsvMultiPovRect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
	bool operator!=(const GdxsvMultiPovRect& o) const { return !(*this == o); }
};

// What the host's window is doing. The guests place themselves relative to it,
// so this is the whole of what "the four screens behave as one window" needs.
struct GdxsvMultiPovHostWindow {
	GdxsvMultiPovRect rect;		 // the host screen's own quadrant, in desktop coords
	GdxsvMultiPovRect group;		 // the area the 2x2 grid covers as a whole
	bool maximized = false;	 // tile the maximized area into four quadrants
	uint32_t generation = 0; // bumped on every change, so a guest can skip no-ops
};

// Playback is host-driven: the guests do not decide where they are, they are
// told. Published every frame by the host.
struct GdxsvMultiPovPlayback {
	int64_t position = 0;		  // key_msg_count * kSyncSubFrames + subframe
	int32_t speed = 0;			  // replay speed index
	bool paused = false;
	uint32_t seek_generation = 0; // bumped per seek, so a guest applies each one once
	int64_t seek_target = 0;	  // key message index the host seeked to
};

// ---- session lifecycle -------------------------------------------------

// A fresh id for a session. Unique per host process, and short enough to sit
// in a temp file name on every platform.
std::string gdxsv_multi_pov_new_session_id();

// Host: creates the session and publishes the replay for the guests to pick
// up. `replay` is the serialised BattleLogFile the host already holds, so a
// guest never re-reads the file or re-downloads it.
bool gdxsv_multi_pov_host_create(const std::string& session_id, const std::vector<uint8_t>& replay);

// Guest: attaches to a session the host created. `screen` is 1..3 (2P..4P).
bool gdxsv_multi_pov_guest_open(const std::string& session_id, int screen);

// Releases the mapping. The host also marks the session closed so any guest
// still running knows to quit.
void gdxsv_multi_pov_close();

GdxsvMultiPovRole gdxsv_multi_pov_current_role();
inline bool gdxsv_multi_pov_active() { return gdxsv_multi_pov_current_role() != GdxsvMultiPovRole::None; }

// 0 for the host (1P), 1..3 for the guests. -1 when not in a session.
int gdxsv_multi_pov_screen_index();

// ---- replay payload ----------------------------------------------------

// Guest: the replay bytes the host published. Waits up to timeout_ms for the
// host to finish publishing; returns false if it never did.
bool gdxsv_multi_pov_fetch_replay(std::vector<uint8_t>& out, int timeout_ms);

// ---- start barrier -----------------------------------------------------
//
// A guest cold-boots the game while the host is already sitting in the menu,
// so the host would otherwise be tens of seconds of playback ahead before the
// first guest drew a frame - far outside the window GdxsvSpectateSync will
// close (peers further away than kSyncEngageWindow are treated as still
// catching up and are not waited for). So the four line up once, here, before
// any of them plays a frame.

// Guest: "I have the replay and I am ready to play". Then waits for the host's
// go signal. Returns false on timeout - playback starts anyway, because a
// screen that hangs is worse than a screen that is late.
bool gdxsv_multi_pov_guest_ready_and_wait(int timeout_ms);

// Host: waits for every spawned guest to report ready, then releases them all.
// `expected_guests` is how many were actually spawned. Returns false if some
// guest never arrived; playback starts regardless, with fewer screens.
bool gdxsv_multi_pov_host_wait_for_guests(int expected_guests, int timeout_ms);

// How many guests have reported ready so far, for a progress readout.
int gdxsv_multi_pov_ready_guest_count();

// How many guests this host actually spawned. 0 outside a host session, so
// `gdxsv_multi_pov_spawned_guest_count() <= gdxsv_multi_pov_ready_guest_count()` reads as "everyone is in" either
// way.
int gdxsv_multi_pov_spawned_guest_count();

// ---- per-frame publication --------------------------------------------

void gdxsv_multi_pov_publish_playback(const GdxsvMultiPovPlayback& state);
bool gdxsv_multi_pov_read_playback(GdxsvMultiPovPlayback& out);

void gdxsv_multi_pov_publish_host_window(const GdxsvMultiPovHostWindow& window);
bool gdxsv_multi_pov_read_host_window(GdxsvMultiPovHostWindow& out);

// Guests: true once the host has closed the session or died, which is the
// signal to shut down - four screens leave together.
bool gdxsv_multi_pov_host_gone();

// Host: true once every guest has gone, so nothing is left behind.
bool gdxsv_multi_pov_guests_gone();

// ---- 2x2 grid geometry -------------------------------------------------

// Splits `group` into the four equal quadrants the screens occupy:
//
//     out[0] = 1P (top-left)     out[1] = 2P (top-right)
//     out[2] = 3P (bottom-left)  out[3] = 4P (bottom-right)
//
// The right and bottom quadrants take the odd pixel when the area does not
// divide evenly, so the four together cover `group` exactly with no seam and
// no overlap - which is what "maximizing tiles them into four equal
// quadrants" has to mean on a 1919-pixel-wide work area.
void gdxsv_multi_pov_compute_grid(const GdxsvMultiPovRect& group, GdxsvMultiPovRect out[kGdxsvMultiPovScreens]);

// ---- host / guest orchestration ----------------------------------------

// True when the user has ticked "4-player replay". Guests never read this:
// they are told what they are on the command line.
bool gdxsv_multi_pov_four_screen_requested();

// Host: reads `replay_source` (a local path or a URL - the host is the only
// one that ever does), publishes it, and spawns the three guests. On success
// `replay_out` holds the bytes for the host's own 1P playback, so the host
// does not read the source twice either.
//
// Returns false when the session could not be set up or the replay is not a
// four-player battle; the caller then falls back to ordinary playback.
bool gdxsv_multi_pov_begin_host_session(const std::string& replay_source, std::vector<uint8_t>& replay_out);

// Guest: joins the session named on the command line and takes the replay
// bytes from the host. False when this process is not a guest.
bool gdxsv_multi_pov_begin_guest_session(std::vector<uint8_t>& replay_out);

// The POV this process plays, from the command line. -1 when not a guest.
int gdxsv_multi_pov_guest_pov();

// The log file this process writes. All four screens share one working
// directory, so a guest names its log after the screen it plays rather than
// writing over the host's.
std::string gdxsv_multi_pov_log_file_name();

// Lines the four screens up once, before the first frame of playback: the
// host waits for its guests, the guests wait for the host's go. A no-op
// outside a session. See the note above gdxsv_multi_pov_guest_ready_and_wait for why this has to
// happen at all.
//
// The host's wait here is short: the long one - a guest has a whole cold boot
// ahead of it - belongs to the UI, which can say "waiting for the other
// screens (n/3)" while it happens instead of freezing the window. By the time
// playback starts the guests are already in, and this only settles the last
// of them.
void gdxsv_multi_pov_wait_at_start_barrier();

