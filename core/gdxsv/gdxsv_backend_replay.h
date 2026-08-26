#pragma once

#include "gdxsv.pb.h"
#include "gdxsv_save_state.h"
#include "gdxsv_spectator_downlink.h"
#include "lbs_message.h"
#include "mcs_message.h"
#include "types.h"

// LBS's lobby port, shared by TCP lobby traffic and the spectator UDP
// channel (see serveUDP in lbs.go). A normal client never needs it spelled
// out: the game itself supplies the port via gdx_rpc's SOCK_OPEN request
// (see Gdxsv::HandleRPC), and only the host comes from config. A spectator
// never runs that lobby-connect flow, so it has nothing to read it from.
constexpr int kGdxsvLbsPort = 9876;

// Mock network implementation to replay local battle log
class GdxsvBackendReplay {
   public:
	enum class State {
		None,
		Start,
		LbsStartBattleFlow,
		McsWaitJoin,
		McsSessionExchange,
		McsInBattle,
		McsWaitStartMsg,
		End,
	};

	void Reset();
	void OnMainUiLoop();
	void OnEndOfFrame();
	void OnNextFrame();
	bool OnOpenMenu();
	void DisplayOSD();

	bool StartFile(const char* path, int pov);
	bool StartBuffer(const std::vector<u8>& buf, int pov);
	// Starts Live Spectate: subscribes to LBS's spectator UDP channel,
	// bootstraps from the header it sends, and then plays back like a normal
	// buffer replay - except it holds at the live edge for more data instead
	// of stopping when it catches up.
	bool StartLive(const std::string& host, const std::string& battle_code, int pov);
	void Stop();
	bool ChangeRoundAvailable() const;

	// Network Backend Interface
	void Open();
	void Close();
	u32 OnSockWrite(u32 addr, u32 size);
	u32 OnSockRead(u32 addr, u32 size);
	u32 OnSockPoll();

   private:
	bool Start();
	bool IsInBriefing() const;
	bool IsInGame() const;
	void PrintDisconnectionSummary() const;
	void ProcessLbsMessage();
	void ProcessMcsMessage(const McsMessage& msg);
	void DeliverKeyMsgBatch();
	void ApplyPatch(bool first_time);
	void RestorePatch();
	void BeginSilentSeek();
	void RunSilentSeekFrame(bool skip_rendering);
	void EndSilentSeek();
	void BeginSilentSeekWithAudioReset();
	void EndSilentSeekWithAudioReset();
	void SendStartMsgs();
	void PrepareRoundStartReplayState();
	void RebuildKeyDisplay() const;
	void BeginLoadingHud();
	void CancelPendingTakeover();
	void RenderPauseMenu();
	void RenderTakeoverAlignment(u16 current_input);
	void RenderTakeoverCountdown();
	void UpdateControlBarVisibility();
	void RenderControlBar();
	void RenderLoadingHud();
	void GetRoundReplayBounds(int& roundStart, int& roundEnd, int& totalRounds) const;
	void GetControlTimelineBounds(int& timelineStart, int& timelineEnd, int& totalRounds) const;
	const char* SpeedText() const;

	// Live Spectate: live_downlink_ (its own background thread) receives
	// pushed deltas from LBS; CheckLiveUpdate (called from OnMainUiLoop,
	// main thread) folds them into log_file_ - the same "background thread
	// stages, main thread mutates" split GdxsvSpectatorUplink uses, so
	// log_file_ itself never needs a lock.
	void CheckLiveUpdate();

	struct ReplayCtrlCommand {
		enum Command {
			None,
			// System used
			SaveFirstFrame,
			SendStartMsg,
			SeekToBriefing,

			// User control
			TogglePauseMenu,
			TogglePause,
			StepFrame,
			StepFrameBackward,
			JumpToKeyMsg,
			SeekForward,
			SeekBackward,
			SetSpeed,
			NextSpeed,
			SetRound,
			NextRound,
			TakeOver,
			StartTakeover,
			RetryTakeover,
			ReturnToReplay,
		};

		ReplayCtrlCommand() = default;
		ReplayCtrlCommand(Command cmd) : cmd(cmd) {}
		ReplayCtrlCommand(Command cmd, int arg1) : cmd(cmd), arg1(arg1) {}
		ReplayCtrlCommand(Command cmd, int arg1, int arg2) : cmd(cmd), arg1(arg1), arg2(arg2) {}

		Command cmd = None;
		int arg1 = 0;
		int arg2 = 0;
	};

	class CommandQueue {
	   public:
		template <class... Args>
		void emplace_back(Args&&... args) {
			std::lock_guard lock(mtx_);
			cmds_.emplace_back(args...);
		}
		void pop_front() {
			std::lock_guard lock(mtx_);
			if (!cmds_.empty()) cmds_.pop_front();
		}
		bool try_get_front(ReplayCtrlCommand& cmd) {
			std::lock_guard lock(mtx_);
			if (!cmds_.empty()) {
				cmd = cmds_.front();
				return true;
			}
			return false;
		}
		size_t size() {
			std::lock_guard lock(mtx_);
			return cmds_.size();
		}
		bool empty() {
			std::lock_guard lock(mtx_);
			return cmds_.empty();
		}
		bool contains(ReplayCtrlCommand::Command c) {
			std::lock_guard lock(mtx_);
			for (const auto& cmd : cmds_) {
				if (cmd.cmd == c) {
					return true;
				}
			}
			return false;
		}
		void clear() {
			std::lock_guard lock(mtx_);
			cmds_.clear();
		}

	   private:
		std::recursive_mutex mtx_;
		std::deque<ReplayCtrlCommand> cmds_;
	};

	State state_ = State::None;
	CommandQueue ctrl_commands_;
	LbsMessageReader lbs_tx_reader_;
	proto::BattleLogFile log_file_;
	std::deque<u8> recv_buf_;
	int pov_ = 0;
	int key_msg_count_ = 0;
	int start_msg_count_ = 0;
	int briefing_start_frame_ = 0;
	int briefing_start_frame_round_ = 0;
	int recv_delay_ = 0;
	bool end_of_frame_ = false;
	bool seeking_ = false;
	int target_round_ = 0;
	int target_frame_ = 0;
	bool pause_menu_opend_ = false;
	bool lbs_first_skip_ = false;
	int ctrl_play_speed_ = 0;
	bool ctrl_step_frame_ = false;
	bool ctrl_pause_ = false;
	bool ctrl_loading_ = false;
	int ctrl_loading_wait_frames_ = 0;
	bool save_converted_log_ = false;

	float ctrl_bar_visibility_ = 0.0f;
	float ctrl_bar_idle_timer_ = 0.0f;
	u32 ctrl_bar_prev_kcode_ = ~0u;
	int audio_fade_frames_ = 0;
	float step_hold_timer_ = 0.0f;
	float flash_left_ = 0.0f;
	float flash_right_ = 0.0f;
	float flash_up_ = 0.0f;
	float flash_down_ = 0.0f;
	float ctrl_bar_prev_mouse_x_ = -1.0f;
	float ctrl_bar_prev_mouse_y_ = -1.0f;
	bool ctrl_bar_dragging_ = false;
	int ctrl_bar_drag_target_frame_ = -1;
	bool ctrl_input_release_pending_ = false;

	// Live Spectate state (see StartLive/CheckLiveUpdate).
	bool live_mode_ = false;
	GdxsvSpectatorDownlink live_downlink_;
	// Latches true once the initial no-render catch-up has closed the gap
	// (see OnNextFrame), after which drift is handled only by the smooth
	// path - so scene transitions never fast-forward.
	bool live_fast_seek_done_ = false;
	int live_fast_seek_count_ = 0;
	// True while the initial jump to the live match's current round is still
	// in flight (queued SeekToBriefing -> SetRound). Live catch-up must not
	// run during that window - see the catch-up gate in OnNextFrame.
	bool live_round_jump_pending_ = false;

	// How far behind the newest available frame playback aims to sit, in
	// frames. Loaded from gdxsv:LiveBufferFrames, default kLiveDefaultBuffer.
	// 1 leaves no cushion at all and will stutter on every arrival clump;
	// 180 holds three seconds back, which is enough that a spectator stream
	// cannot be used to call out an opponent's position in real time.
	int live_buffer_frames_ = 30;

	// Steers the main loop's frame period so playback holds live_buffer_frames_
	// behind the edge. Small, continuous corrections instead of whole-frame
	// stalls - see gdxsv_frame_period_trim_us.
	void UpdateFramePacing();

	bool takeover_ = false;
	int takeover_saved_frame_ = -1;
	int takeover_countdown_ = 0;
	bool takeover_aligning_ = false;
	u16 takeover_target_input_ = 0;
	std::deque<u16> takeover_input_buf_;
};