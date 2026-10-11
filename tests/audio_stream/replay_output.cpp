#include "../inc/StandaloneCheck.h"
#include "audio/audiostream.cpp"

static GdxsvMultiPovRole role = GdxsvMultiPovRole::Host;
static int guestPov = -1;
static uint32_t lastCount, silentPackets, delayedCount, roleQueries;
GdxsvMultiPovRole gdxsv_multi_pov_current_role() { ++roleQueries; return role; }
int gdxsv_multi_pov_guest_pov() { return guestPov; }
void gdxsv_multi_pov_audio_enable(bool) {}
void gdxsv_multi_pov_audio_reset() {}
uint32_t gdxsv_multi_pov_audio_frame(int64_t, uint32_t,
		const GdxsvMultiPovAudioSample* samples, uint32_t count,
		GdxsvMultiPovAudioSample* mixed, uint32_t) {
	lastCount = count;
	if (delayedCount != 0) {
		std::fill_n(mixed, delayedCount, GdxsvMultiPovAudioSample{5000, -5000});
		return delayedCount;
	}
	if (count == 0) { ++silentPackets; return 0; }
	GdxsvMultiPovAudioPacket packets[4] {};
	for (auto& packet : packets) {
		packet.count = count;
		std::copy_n(samples, count, packet.samples);
	}
	gdxsv_multi_pov_mix_audio(packets, 15, mixed, count);
	return role == GdxsvMultiPovRole::Guest ? 0 : count;
}

class Device : public AudioBackend {
public:
	Device() : AudioBackend("test", "test") {}
	std::vector<s16> pcm;
	unsigned queries = 0, restarts = 0;
	bool canFlush = true;
	bool prebuffering = false;
	bool init() override { ++restarts; return true; }
	u32 push(const void* data, u32 frames, bool wait) override {
		CHECK(!wait);
		const auto* samples = static_cast<const s16*>(data);
		pcm.insert(pcm.end(), samples, samples + frames * 2);
		return 1;
	}
	bool getQueueLevel(u32& queued, u32& capacity) override {
		++queries;
		queued = 0; capacity = 4096;
		return true;
	}
	bool flush() override { return canFlush; }
	void setPrebuffering(bool enabled) override { prebuffering = enabled; }
};

static void frame() {
	for (int i = 0; i < 736; ++i) WriteSample(5000 + i, -5000 - i);
}

int main() {
	Device device;
	InitAudio();
	CHECK(!device.prebuffering);
	FlushAudio();
	const auto initialRoleQueries = roleQueries;
	for (int i = 0; i < 100; ++i) frame();
	CHECK(roleQueries == initialRoleQueries); // process role is cached, not queried per sample
	const auto single = device.pcm;
	CHECK(device.queries > 0);
	CHECK(single.size() / 2 > 73600); // an empty device needs more output PCM
	device.pcm.clear();
	device.queries = 0;
	SetMultiPovAudio(true);
	CHECK(device.prebuffering);
	for (int i = 0; i < 100; ++i) {
		frame();
		FinishMultiPovAudioFrame(i, 0, true);
	}
	CHECK(device.queries > 0);
	CHECK(device.pcm == single); // exactly one resampler, after the shared mix
	const auto audibleSize = device.pcm.size();
	frame();
	FinishMultiPovAudioFrame(100, 0, false);
	CHECK(lastCount == 0 && silentPackets == 1);
	FinishMultiPovAudioFrame(101, 0, true); // an empty audible frame also acknowledges
	CHECK(lastCount == 0 && silentPackets == 2);
	CHECK(device.pcm.size() == audibleSize);
	settings.gdxsv.audioRateControl = false;
	device.pcm.clear();
	FlushAudio();
	delayedCount = 735;
	FinishMultiPovAudioFrame(102, 0, false);
	CHECK(lastCount == 0 && device.pcm.size() / 2 + writePtr == 735); // output during a silent burst
	device.pcm.clear();
	FlushAudio();
	frame();
	FinishMultiPovAudioFrame(103, 0, true);
	CHECK(lastCount == 736 && device.pcm.size() / 2 + writePtr == 735); // delayed output owns its sample length
	delayedCount = 0;
	settings.gdxsv.audioRateControl = true;
	FlushAudio();
	for (int i = 0; i < 4; ++i) frame();
	FinishMultiPovAudioFrame(102, 0, true);
	CHECK(lastCount == 4 * 736); // a long hook interval retains more than 2048 samples
	settings.gdxsv.audioScale = 0.f;
	device.pcm.clear();
	FlushAudio();
	frame();
	FinishMultiPovAudioFrame(102, 0, true);
	CHECK(!device.pcm.empty());
	for (auto sample : device.pcm) CHECK(sample == 0);
	device.canFlush = false;
	const auto restarts = device.restarts;
	FlushAudio();
	CHECK(device.restarts == restarts + 1); // fallback discards other backend queues
	settings.gdxsv.audioScale = 1.f;
	device.pcm.clear();
	EventManager::event(Event::Terminate);
	CHECK(!device.prebuffering);
	frame();
	CHECK(!device.pcm.empty()); // unload restores direct output without Stop()
	SetMultiPovAudio(true);
	device.pcm.clear();
	settings.gdxsv.replayModeActive = false;
	EventManager::event(Event::LoadState);
	CHECK(!device.prebuffering);
	frame();
	CHECK(!device.pcm.empty()); // loading a non-replay state also restores output
	// Non-SDL mode transitions reopen once, and guests remain device-free
	// across Reset/Stop/unload and the interval between live battles.
	SetMultiPovAudio(false);
	const auto modeRestarts = device.restarts;
	SetMultiPovAudio(true);
	CHECK(device.restarts == modeRestarts + 1);
	role = GdxsvMultiPovRole::Guest;
	InitAudio();
	CHECK(currentBackend == nullptr);
	const auto guestRestarts = device.restarts;
	for (int cycle = 0; cycle < 3; ++cycle) {
		SetMultiPovAudio(false);
		InitAudio();
		frame();
		CHECK(currentBackend == nullptr && device.restarts == guestRestarts);
		SetMultiPovAudio(true);
		frame();
		FinishMultiPovAudioFrame(200 + cycle, 0, true);
		EventManager::event(Event::Terminate);
		CHECK(currentBackend == nullptr && device.restarts == guestRestarts);
	}
	role = GdxsvMultiPovRole::None;
	guestPov = 1; // the host died and the mapping closed before guest teardown
	InitAudio();
	SetMultiPovAudio(false);
	frame();
	CHECK(currentBackend == nullptr && device.restarts == guestRestarts);
	guestPov = -1;
	InitAudio();
	CHECK(currentBackend != nullptr && device.restarts == guestRestarts + 1);
	TermAudio();
	const auto stoppedRestarts = device.restarts;
	SetMultiPovAudio(true);
	CHECK(currentBackend == nullptr && device.restarts == stoppedRestarts);
	InitAudio(); // Emulator::start initializes once, after the mode was set
	CHECK(device.restarts == stoppedRestarts + 1);
	SetMultiPovAudio(false);
	TermAudio();
}
