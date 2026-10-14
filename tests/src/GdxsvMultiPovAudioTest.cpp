// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtest/gtest.h"

#if GTEST_HAS_DEATH_TEST && !defined(_WIN32) && !defined(LIBRETRO)
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>

#include "audio/audiostream.h"
#include "cfg/option.h"
#include "gdxsv/gdxsv_multi_pov.h"
#include "gdxsv/gdxsv_multi_pov_audio.h"

namespace {
struct GdxsvAudioTestSample {
	int16_t left, right;
};

class GdxsvAudioTestOutput : public AudioBackend {
   public:
	GdxsvAudioTestOutput() : AudioBackend("gdxsv-test-output", "Test PCM capture") {}
	bool init() override { return true; }
	u32 push(const void* data, u32 count, bool wait) override {
		EXPECT_FALSE(wait);
		const auto* samples = static_cast<const GdxsvAudioTestSample*>(data);
		pcm.insert(pcm.end(), samples, samples + count);
		return 1;
	}
	std::vector<GdxsvAudioTestSample> pcm;
};

struct GdxsvAudioTestCommand {
	int input = 0, speed = 0;
	bool silent = false, request = false, misaligned = false;
	int marker_frame = -1;
};

class GdxsvAudioTestSession {
   public:
	~GdxsvAudioTestSession() {
		for (auto& child : children_) {
			if (child.fd >= 0) close(child.fd);
			if (child.pid > 0) {
				kill(child.pid, SIGKILL);
				int status;
				while (waitpid(child.pid, &status, 0) < 0 && errno == EINTR) {
				}
			}
		}
		TermAudio();
		gdxsv_multi_pov_close();
	}

	bool Start() {
		config::setTransient("gdxsv", "headless", "no");
		config::setTransient("gdxsv", "MultiPovAudioSync", "yes");
		config::setTransient("gdxsv", "MultiPovMixVolume", "100");
		config::setTransient("gdxsv", "AudioRateControl", "no");
		config::AudioBackend.override(output.slug);
		config::AudioVolume.logarithmic_volume_scale = .5f;
		settings.gdxsv.replayModeActive = true;
		const auto session = gdxsv_multi_pov_new_session_id();
		// Fork before creating the host mapping: a child must join as a guest,
		// without inheriting a host session that its open would close.
		for (int i = 0; i < 3; ++i) {
			int sockets[2];
			if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return false;
			const pid_t pid = fork();
			if (pid < 0) {
				close(sockets[0]);
				close(sockets[1]);
				return false;
			}
			if (pid == 0) {
				close(sockets[0]);
				for (int j = 0; j < i; ++j) close(children_[j].fd);
				Guest(session, i + 1, sockets[1]);
			}
			close(sockets[1]);
			children_[i] = {pid, sockets[0]};
		}
		// The session transports opaque replay bytes; these tests supply PCM
		// directly and do not load a game or parse a replay.
		if (!gdxsv_multi_pov_host_create(session, {0}) || !gdxsv_multi_pov_audio_enabled()) return false;
		gdxsv_multi_pov_audio_start();
		InitAudio();
		for (int i = 0; i < 3; ++i)
			if (!Send(i, {-1})) return false;
		return true;
	}

	bool Send(int guest, const GdxsvAudioTestCommand& command) {
		const int fd = children_[guest].fd;
		if (write(fd, &command, sizeof(command)) != sizeof(command)) {
			ADD_FAILURE() << "guest command write failed";
			return false;
		}
		char reply;
		if (!Read(fd, &reply, sizeof(reply))) {
			ADD_FAILURE() << "guest did not complete its frame";
			return false;
		}
		return reply != 0;
	}

	bool Frame(const GdxsvAudioTestCommand& command, int screen = 0) {
		if (command.input < 0) {
			gdxsv_multi_pov_audio_reset();
			frames_ = 0;
			return true;
		}
		if (command.request) return gdxsv_multi_pov_audio_input_ready(command.input, 1, 0, true);
		if (!command.silent) {
			for (int j = 0; j < 735; ++j) {
				const int point = j - (command.misaligned && screen == 1 ? 8 : 0);
				int amplitude = point == 64 ? 8000 : (point == 128 + screen * 32 ? 4000 : 0);
				if (command.marker_frame >= 0) amplitude = point == 64 && frames_ == command.marker_frame ? 8000 : 0;
				WriteSample(-amplitude, amplitude);
			}
		}
		gdxsv_multi_pov_audio_frame(command.input, 1, 0, command.speed, command.silent);
		++frames_;
		return true;
	}

	GdxsvAudioTestOutput output;

   private:
	static bool Read(int fd, void* data, size_t size) {
		auto* bytes = static_cast<char*>(data);
		while (size != 0) {
			pollfd poll_fd{fd, POLLIN, 0};
			if (poll(&poll_fd, 1, 5000) <= 0) return false;
			const auto n = read(fd, bytes, size);
			if (n <= 0) return false;
			bytes += n;
			size -= n;
		}
		return true;
	}

	[[noreturn]] void Guest(const std::string& session, int screen, int fd) {
		GdxsvAudioTestCommand command;
		if (!Read(fd, &command, sizeof(command)) || !gdxsv_multi_pov_guest_open(session, screen) || !gdxsv_multi_pov_audio_enabled())
			_exit(1);
		gdxsv_multi_pov_audio_start();
		InitAudio();
		do {
			const char reply = Frame(command, screen);
			if (!output.pcm.empty() || write(fd, &reply, sizeof(reply)) != sizeof(reply)) _exit(1);
		} while (Read(fd, &command, sizeof(command)));
		TermAudio();
		gdxsv_multi_pov_close();
		_exit(::testing::Test::HasFailure() ? 1 : 0);
	}

	struct Child {
		pid_t pid = -1;
		int fd = -1;
	};
	std::array<Child, 3> children_;
	int frames_ = 0;
};

bool Aligned(const std::vector<GdxsvAudioTestSample>& pcm) {
	if (pcm.size() <= 110 * 735) return false;
	for (size_t j = 0; j < pcm.size(); ++j) {
		const int offset = j == 0 ? -1 : (j - 1) % 735;
		const int expected = offset == 64 ? 4000 : (offset >= 128 && offset <= 224 && (offset - 128) % 32 == 0 ? 500 : 0);
		if (pcm[j].left != expected || pcm[j].right != -expected) return false;
	}
	return true;
}

void Alignment(GdxsvAudioTestSession& session, int speed, bool misaligned) {
	const int stride = speed + 1;
	for (int n = 0; n < 120 * stride; ++n) {
		for (int i = 0; i < 3; ++i) {
			const int input = n - i * stride;
			if (input >= 0) ASSERT_TRUE(session.Send(i, {input, speed, false, false, misaligned}));
		}
		session.Frame({n, speed, n % stride != 0});
	}
	EXPECT_GT(session.output.pcm.size(), 110u * 735);
	EXPECT_EQ(Aligned(session.output.pcm), !misaligned);
}

void QueueRecovery(GdxsvAudioTestSession& session) {
	// An ahead guest produces seconds of audio while the host consumes none,
	// then rewinds behind those queued positions.
	for (int n = 1000; n < 1240; ++n) ASSERT_TRUE(session.Send(0, {n}));
	for (int i = 0; i < 3; ++i) ASSERT_TRUE(session.Send(i, {-1}));
	session.Frame({-1});
	for (int n = 0; n < 120; ++n) {
		for (int i = 0; i < 3; ++i) ASSERT_TRUE(session.Send(i, {n}));
		session.Frame({n});
	}
	// The full queue first discards its pre-rewind packets. The remaining
	// output must recover all four copies of the shared impulse.
	EXPECT_GE(std::count_if(session.output.pcm.begin(), session.output.pcm.end(),
							[](const auto& sample) { return sample.left == 4000 && sample.right == -4000; }),
			  110);
}

void PausedRestore(GdxsvAudioTestSession& session) {
	// Sound is tied to time since each restore, not time since the host's
	// restore. Late guests must not replay that marker against newer audio.
	for (int n = 0; n < 1300; ++n) {
		if (n == 1100)
			for (int i = 0; i < 3; ++i) ASSERT_TRUE(session.Send(i, {-1}));
		session.Frame({500, 0, false, false, false, 50});
		for (int i = 0; i < 3; ++i)
			if (n >= 13 + 7 * i && !(n >= 1100 && n < 1113 + 7 * i)) ASSERT_TRUE(session.Send(i, {500, 0, false, false, false, 50}));
	}
	ASSERT_GT(session.output.pcm.size(), 1250u * 735);
	int markers = 0;
	for (size_t j = 0; j < session.output.pcm.size(); ++j) {
		const auto& sample = session.output.pcm[j];
		if (sample.left == 0 && sample.right == 0) continue;
		++markers;
		ASSERT_EQ(j, 50u * 735 + 65) << "a late paused restore replayed an earlier sound";
		EXPECT_EQ(sample.left, 1000);
		EXPECT_EQ(sample.right, -1000);
	}
	EXPECT_EQ(markers, 1);
}

void BriefingRequests(GdxsvAudioTestSession& session) {
	for (int n = 0; n < 10; ++n) {
		for (int i = 0; i < 3; ++i) ASSERT_TRUE(session.Send(i, {500}));
		session.Frame({500});
	}
	session.output.pcm.clear();
	ASSERT_FALSE(session.Send(1, {500, 0, false, true}));
	ASSERT_FALSE(session.Send(2, {500, 0, false, true}));
	ASSERT_FALSE(gdxsv_multi_pov_audio_input_ready(500, 1, 0, true));
	for (int n = 0; n < 13; ++n) {
		session.Frame({500});
		for (int i = 0; i < 3; ++i) ASSERT_TRUE(session.Send(i, {500}));
	}
	EXPECT_GT(session.output.pcm.size(), 10u * 735);
	ASSERT_TRUE(session.Send(0, {500, 0, false, true}));
	EXPECT_TRUE(gdxsv_multi_pov_audio_input_ready(500, 1, 0, true));
	for (int i = 1; i < 3; ++i) EXPECT_TRUE(session.Send(i, {500, 0, false, true}));

	// Keep the peers alive without submitting the next request.
	const auto started = std::chrono::steady_clock::now();
	ASSERT_FALSE(gdxsv_multi_pov_audio_input_ready(501, 1, 0, true));
	while (!gdxsv_multi_pov_audio_input_ready(501, 1, 0, true)) {
		for (int i = 0; i < 3; ++i) ASSERT_TRUE(session.Send(i, {500}));
		session.Frame({500});
		usleep(1000);
		ASSERT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
	}
	EXPECT_FALSE(gdxsv_multi_pov_audio_input_pending());
}

// Isolate backend registration and transient configuration from other tests.
// The three workers then use the real shared session and linked audio code.
[[noreturn]] void RunAudioCheck(void (*check)(GdxsvAudioTestSession&)) {
	alarm(20);
	{
		GdxsvAudioTestSession session;
		if (session.Start())
			check(session);
		else
			ADD_FAILURE() << "could not create the audio session";
	}
	std::fflush(stdout);
	_exit(::testing::Test::HasFailure() ? 1 : 0);
}
}  // namespace

class GdxsvMultiPovAudioTest : public ::testing::Test {
	void SetUp() override { GTEST_FLAG_SET(death_test_style, "threadsafe"); }
};

TEST_F(GdxsvMultiPovAudioTest, AlignsAt1x) {
	ASSERT_EXIT(RunAudioCheck([](auto& session) { Alignment(session, 0, false); }), ::testing::ExitedWithCode(0), "");
}
TEST_F(GdxsvMultiPovAudioTest, AlignsAt2x) {
	ASSERT_EXIT(RunAudioCheck([](auto& session) { Alignment(session, 1, false); }), ::testing::ExitedWithCode(0), "");
}
TEST_F(GdxsvMultiPovAudioTest, AlignsAt3x) {
	ASSERT_EXIT(RunAudioCheck([](auto& session) { Alignment(session, 2, false); }), ::testing::ExitedWithCode(0), "");
}
TEST_F(GdxsvMultiPovAudioTest, RejectsDelayedCopy) {
	ASSERT_EXIT(RunAudioCheck([](auto& session) { Alignment(session, 0, true); }), ::testing::ExitedWithCode(0), "");
}
TEST_F(GdxsvMultiPovAudioTest, RecoversAfterAnAheadGuestRewinds) {
	ASSERT_EXIT(RunAudioCheck(QueueRecovery), ::testing::ExitedWithCode(0), "");
}
TEST_F(GdxsvMultiPovAudioTest, PlaysPausedRestoreMarkerOnce) {
	ASSERT_EXIT(RunAudioCheck(PausedRestore), ::testing::ExitedWithCode(0), "");
}
TEST_F(GdxsvMultiPovAudioTest, BriefingWaitKeepsAudioPlayingAndExpires) {
	ASSERT_EXIT(RunAudioCheck(BriefingRequests), ::testing::ExitedWithCode(0), "");
}
#else
TEST(GdxsvMultiPovAudioTest, RequiresFork) { GTEST_SKIP() << "Cross-process audio checks require POSIX fork and googletest death tests."; }
#endif
