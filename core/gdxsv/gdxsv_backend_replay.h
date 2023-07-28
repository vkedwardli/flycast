#pragma once

#include "gdxsv.pb.h"
#include "gdxsv_save_state.h"
#include "lbs_message.h"
#include "mcs_message.h"
#include "types.h"

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
		End,
	};

	void Reset();
	void OnMainUiLoop();
	void OnVBlank();
	bool OnOpenMenu();
	void DisplayOSD();

	bool StartFile(const char *path, int pov);
	bool StartBuffer(const std::vector<u8> &buf, int pov);
	void Stop();

	// Replay control
	void CtrlSpeedUp();
	void CtrlSpeedDown();
	void CtrlTogglePause();
	void CtrlStepFrame();
	void CtrlSomeFrameBackward();
	void CtrlSomeFrameForward();
	void CtrlChangeRound(int round);

	// Network Backend Interface
	void Open();
	void Close();
	u32 OnSockWrite(u32 addr, u32 size);
	u32 OnSockRead(u32 addr, u32 size);
	u32 OnSockPoll();

   private:
	bool Start();
	void PrintDisconnectionSummary();
	void ProcessLbsMessage();
	void ProcessMcsMessage(const McsMessage &msg);
	void ApplyPatch(bool first_time);
	void RestorePatch();
	void RenderPauseMenu();

	State state_;
	bool pause_menu_opend_;
	LbsMessageReader lbs_tx_reader_;
	proto::BattleLogFile log_file_;
	std::deque<u8> recv_buf_;
	int recv_delay_;
	int seek_frames_;
	int start_msg_count_;
	int pov_;
	std::atomic<int> key_msg_count_;
	std::atomic<bool> start_msg_received_;
	std::atomic<int> ctrl_play_speed_;
	std::atomic<bool> ctrl_pause_;
	std::atomic<bool> ctrl_step_frame_;
	std::atomic<bool> ctrl_some_frame_backward_;
	std::atomic<bool> ctrl_some_frame_forward_;
	std::atomic<int> ctrl_change_round_;
	bool save_converted_log_;
};