#include <array>
#include "../inc/StandaloneCheck.h"
#define USE_SDL_AUDIO
#include "audio/audiobackend_sdl2.cpp"

std::vector<AudioBackend*>* AudioBackend::backends;

static void push(SDLAudioBackend& backend, uint32_t value) {
	std::array<uint32_t, 512> samples;
	samples.fill(value);
	backend.push(samples.data(), samples.size(), false);
	backend.push(samples.data(), samples.size(), false);
}

static void callback(uint32_t expected) {
	std::array<uint32_t, 1024> output;
	output.fill(0xcafef00d);
	outputSpec.callback(outputSpec.userdata, reinterpret_cast<Uint8*>(output.data()), sizeof(output));
	for (auto sample : output) CHECK(sample == expected);
}

static uint32_t queued(SDLAudioBackend& backend) {
	u32 count, capacity;
	CHECK(backend.getQueueLevel(count, capacity));
	return count;
}

int main() {
	SDLAudioBackend backend;
	CHECK(backend.init());
	backend.setPrebuffering(true);
	push(backend, 555);
	callback(0);
	CHECK(queued(backend) == 1024); // priming retains real PCM
	push(backend, 555);
	callback(555);
	CHECK(queued(backend) == 1024); // headroom exists immediately
	callback(555);
	callback(0); // a later underrun rearms priming
	push(backend, 555);
	callback(0);
	CHECK(queued(backend) == 1024);
	push(backend, 555);
	callback(555);
	CHECK(backend.flush()); // seeking removes old samples and rearms priming
	push(backend, 666);
	callback(0);
	push(backend, 666);
	callback(666);
	backend.setPrebuffering(false);
	CHECK(backend.flush());
	push(backend, 777);
	callback(777); // single-view/normal output keeps its original start threshold
	callback(0);
	push(backend, 888);
	callback(888); // normal underruns also retain their original behavior
	backend.term();
	config::AudioBufferSize = 1024;
	CHECK(backend.init());
	backend.setPrebuffering(true);
	push(backend, 999);
	callback(999); // priming never waits for more PCM than the queue can hold
	backend.term();
}
