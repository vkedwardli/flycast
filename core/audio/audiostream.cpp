#include "audiostream.h"
#include "cfg/option.h"
#include "emulator.h"
#include "gdxsv/gdxsv_multi_pov.h"

#include <algorithm>

static void registerForEvents();

using SoundFrame = GdxsvMultiPovAudioSample;

static SoundFrame Buffer[SAMPLE_COUNT];
static u32 writePtr;  // next sample index
static bool multiPovAudio;
static SoundFrame multiPovFrame[kMultiPovAudioSamples];
static u32 multiPovFrameCount;

static AudioBackend *currentBackend;
std::vector<AudioBackend *> *AudioBackend::backends;

static bool audio_recording_started;
static bool eight_khz;
static bool multiPovGuestProcess;

static bool isMultiPovGuest()
{
	// Command-line identity survives session close, including host death.
	return multiPovGuestProcess || gdxsv_multi_pov_current_role() == GdxsvMultiPovRole::Guest;
}

AudioBackend *AudioBackend::getBackend(const std::string& slug)
{
	if (backends == nullptr)
		return nullptr;
	if (slug == "auto")
	{
		// Prefer sdl2 if available and avoid the null driver
		AudioBackend *sdlBackend = nullptr;
		AudioBackend *autoBackend = nullptr;
		for (auto backend : *backends)
		{
			if (backend->slug == "sdl2")
				sdlBackend = backend;
			if (backend->slug != "null" && autoBackend == nullptr)
				autoBackend = backend;
		}
		if (sdlBackend != nullptr)
			autoBackend = sdlBackend;
		if (autoBackend == nullptr)
			autoBackend = backends->front();
		INFO_LOG(AUDIO, "Auto-selected audio backend \"%s\" (%s).", autoBackend->slug.c_str(), autoBackend->getName().c_str());

		return autoBackend;
	}
	for (auto backend : *backends)
	{
		if (backend->slug == slug)
			return backend;
	}
	WARN_LOG(AUDIO, "WARNING: Audio backend \"%s\" not found!", slug.c_str());
	return nullptr;
}

// Audio rate control, for replay. It runs without audio sync, as the online
// battle does, so the emulator's sample rate drifts from the device's 44.1kHz
// with the playback pacing, and the queue overflows or runs dry - either one
// clicks. Resampling by up to 0.5% to hold the queue near half full absorbs
// that without an audible pitch change (RetroArch's dynamic rate control).
namespace {
constexpr double kRateMaxCorrection = 0.005;
// Smoothing per push (~12ms). The SDL2 device takes 1024 frames per callback,
// so the queue level is a sawtooth of that size.
constexpr double kQueueAlpha = 0.05;

struct RateControl
{
	bool active = false;
	SoundFrame prev {};
	double phase = 0.0;	// position of the next output frame after prev, in input frames
	double step = 1.0;	// input frames per output frame
	double queueLevel = 0.5;
};
RateControl rateControl;
}

static void updateRate()
{
	RateControl& rc = rateControl;
	u32 queued, capacity;
	if (currentBackend == nullptr || !currentBackend->getQueueLevel(queued, capacity) || capacity == 0)
		return;
	rc.queueLevel += kQueueAlpha * (std::min<double>(1.0, (double)queued / capacity) - rc.queueLevel);
	rc.step = 1.0 / (1.0 + kRateMaxCorrection * (1.0 - 2.0 * rc.queueLevel));
}

static void pushFrame(const SoundFrame& frame)
{
	Buffer[writePtr] = frame;
	if (++writePtr == SAMPLE_COUNT)
	{
		if (currentBackend != nullptr)
			currentBackend->push(Buffer, SAMPLE_COUNT, config::LimitFPS);
		writePtr = 0;
		if (rateControl.active)
			updateRate();
	}
}

// Both single-view PCM and the final four-view mix use the same device-clock
// correction. Resample after mixing so the viewpoints never drift apart.
static void outputFrame(const SoundFrame& frame)
{
	RateControl& rc = rateControl;
	if (rc.active != settings.gdxsv.audioRateControl) {
		rc = {};
		rc.active = settings.gdxsv.audioRateControl;
		rc.prev = frame;
	}
	if (!rc.active) {
		pushFrame(frame);
		return;
	}

	// Linear interpolation between the previous input frame and this one.
	while (rc.phase < 1.0)
	{
		const float t = (float)rc.phase;
		SoundFrame out;
		out.l = (s16)(rc.prev.l + (frame.l - rc.prev.l) * t);
		out.r = (s16)(rc.prev.r + (frame.r - rc.prev.r) * t);
		pushFrame(out);
		rc.phase += rc.step;
	}
	rc.phase -= 1.0;
	rc.prev = frame;
}

void WriteSample(s16 r, s16 l)
{
	if (!multiPovAudio && multiPovGuestProcess)
		return; // the host owns output even between battles and during teardown
	float vol = config::AudioVolume.dbPower() * settings.aica.audioFade;
	if (multiPovAudio || (0.f < settings.gdxsv.audioScale && settings.gdxsv.audioScale < 1.f))
		vol *= settings.gdxsv.audioScale;
	SoundFrame frame;
	frame.r = r * vol;
	frame.l = l * vol;
	if (multiPovAudio) {
		if (multiPovFrameCount < kMultiPovAudioSamples)
			multiPovFrame[multiPovFrameCount++] = frame;
		return;
	}
	outputFrame(frame);
}

void InitAudio()
{
	registerForEvents();
	TermAudio();
	multiPovGuestProcess = gdxsv_multi_pov_guest_pov() >= 0 || gdxsv_multi_pov_current_role() == GdxsvMultiPovRole::Guest;
	// Guests send PCM to the host; a second device would reintroduce echo.
	if (isMultiPovGuest())
		return;

	std::string slug = config::AudioBackend;
	currentBackend = AudioBackend::getBackend(slug);
	if (currentBackend == nullptr && slug != "auto")
	{
		slug = "auto";
		currentBackend = AudioBackend::getBackend(slug);
	}
	if (currentBackend != nullptr)
	{
		INFO_LOG(AUDIO, "Initializing audio backend \"%s\" (%s)...", currentBackend->slug.c_str(), currentBackend->getName().c_str());
		if (!currentBackend->init())
		{
			currentBackend = nullptr;
			if (slug != "auto")
			{
				WARN_LOG(AUDIO, "Audio driver %s failed to initialize. Defaulting to 'auto'", slug.c_str());
				slug = "auto";
				currentBackend = AudioBackend::getBackend(slug);
				if (!currentBackend->init())
					currentBackend = nullptr;
			}
		}
	}

	if (currentBackend == nullptr)
	{
		WARN_LOG(AUDIO, "Running without audio!");
		return;
	}

	currentBackend->setPrebuffering(multiPovAudio);

	if (audio_recording_started)
	{
		// Restart recording
		audio_recording_started = false;
		StartAudioRecording(eight_khz);
	}
}

void TermAudio()
{
	if (currentBackend == nullptr)
		return;

	// Save recording state before stopping
	bool rec_started = audio_recording_started;
	StopAudioRecording();
	audio_recording_started = rec_started;
	currentBackend->term();
	INFO_LOG(AUDIO, "Terminating audio backend \"%s\" (%s)...", currentBackend->slug.c_str(), currentBackend->getName().c_str());
	currentBackend = nullptr;
}

void FlushAudio()
{
	writePtr = 0;
	multiPovFrameCount = 0;
	rateControl = {};
	if (multiPovAudio) gdxsv_multi_pov_audio_reset();
	if (multiPovAudio && currentBackend != nullptr && !currentBackend->flush())
		InitAudio(); // CoreAudio, DirectSound, etc. also discard pre-seek PCM
}

void SetMultiPovAudio(bool enabled)
{
	if (multiPovAudio == enabled) return;
	multiPovAudio = enabled;
	gdxsv_multi_pov_audio_enable(enabled);
	if (currentBackend != nullptr) currentBackend->setPrebuffering(enabled);
	FlushAudio();
	// Emulator::start owns device initialization. A mode change with no
	// device must not open one just before start opens it again.
}

void FinishMultiPovAudioFrame(int64_t position, uint32_t seek_generation, bool audible, uint32_t timeline)
{
	if (!multiPovAudio) return;
	const u32 count = multiPovFrameCount;
	multiPovFrameCount = 0;
	static SoundFrame mixed[kMultiPovAudioSamples];
	// Silent frames still acknowledge their replay position and let the host
	// drain old guest audio without waiting for samples that will never arrive.
	const u32 mixed_count = gdxsv_multi_pov_audio_frame(position, seek_generation, multiPovFrame, audible ? count : 0, mixed, timeline);
	for (u32 i = 0; i < mixed_count; ++i) outputFrame(mixed[i]);
}

void StartAudioRecording(bool eight_khz)
{
	::eight_khz = eight_khz;
	if (currentBackend != nullptr)
		audio_recording_started = currentBackend->initRecord(eight_khz ? 8000 : 11025);
	else
		// might be called between TermAudio/InitAudio
		audio_recording_started = true;
}

u32 RecordAudio(void *buffer, u32 samples)
{
	if (!audio_recording_started || currentBackend == nullptr)
		return 0;
	return currentBackend->record(buffer, samples);
}

void StopAudioRecording()
{
	// might be called between TermAudio/InitAudio
	if (audio_recording_started && currentBackend != nullptr)
		currentBackend->termRecord();
	audio_recording_started = false;
}

static void registerForEvents()
{
	static bool done;
	if (done)
		return;
	done = true;
	// Empty the audio buffer when loading a state or terminating the game
	const auto& callback = [](Event event, void *) {
		if (multiPovAudio && (event == Event::Terminate || !settings.gdxsv.replayModeActive))
			SetMultiPovAudio(false);
		FlushAudio();
	};
	EventManager::listen(Event::Terminate, callback);
	EventManager::listen(Event::LoadState, callback);
}


