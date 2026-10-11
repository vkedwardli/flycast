#include "../inc/StandaloneCheck.h"
#include <new>
#include <thread>
#include <sys/wait.h>
#include "gdxsv/gdxsv_multi_pov_audio.h"
#include "gdxsv/gdxsv_spectate_sync.cpp"

struct Shared {
	GdxsvMultiPovAudioQueue queue;
	std::atomic<bool> ready {false}, done {false};
};

int main() {
	constexpr int first_frame = 10;
	// Require two queue drains even with the video wait's two-frame slack.
	constexpr int frame_count = 2 * kMultiPovAudioPackets + 4;
	constexpr int ready_frame = first_frame + kMultiPovAudioPackets - 1;
	constexpr int last_frame = first_frame + frame_count - 1;
	void* mapping = mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
	CHECK(mapping != MAP_FAILED);
	auto* shared = new (mapping) Shared;
	const std::string group = "audio_recovery_test_" + std::to_string(getpid());
	GdxsvSpectateSync host;
	host.Join(group);
	CHECK(host.Active());
	const pid_t child = fork();
	CHECK(child >= 0);
	if (child == 0) {
		GdxsvSpectateSync guest;
		guest.Join(group);
		guest.Publish(first_frame * 1024, false);
		GdxsvMultiPovAudioPacket packet;
		packet.epoch = 1;
		packet.count = 1;
		packet.samples[0] = {1234, -1234};
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		for (int frame = first_frame; frame <= last_frame; ++frame) {
			packet.position = int64_t(frame) << 32;
			while (!shared->queue.push(packet)) {
				if (std::chrono::steady_clock::now() >= deadline) _exit(1);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			guest.Publish(frame * 1024, false);
			if (frame == ready_frame) shared->ready.store(true);
		}
		while (!shared->done.load()) {
			if (std::chrono::steady_clock::now() >= deadline) _exit(2);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		guest.Leave();
		_exit(0);
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!shared->ready.load() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	CHECK(shared->ready.load());
	CHECK(shared->queue.write.load() - shared->queue.read.load() == kMultiPovAudioPackets);
	unsigned maintenance_calls = 0;
	const bool caught_up = host.WaitForPeers(last_frame * 1024, 500, [&]() {
		++maintenance_calls;
		shared->queue.discardBefore(1, int64_t(last_frame) << 32);
	});
	shared->done.store(true);
	int status;
	const pid_t waited = waitpid(child, &status, 0);
	CHECK(waited == child);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	CHECK(caught_up);
	CHECK(maintenance_calls >= 2); // the guest filled its queue again during the wait
	GdxsvMultiPovAudioPacket packet;
	bool future;
	CHECK(shared->queue.take(1, int64_t(last_frame) << 32, packet, future));
	CHECK(packet.samples[0].l == 1234); // the matching frame survived synchronization
	host.Leave();
	munmap(mapping, sizeof(Shared));
}
