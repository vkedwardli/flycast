// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>

#include "audio/audiostream.h"
#include "gdxsv_audio_hooks.h"

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

struct Frame {
	s16 l;
	s16 r;
};

struct RateControl {
	bool active = false;
	Frame prev{};
	double phase = 0.0;	 // position of the next output frame after prev, in input frames
	double step = 1.0;	 // input frames per output frame
	double queueLevel = 0.5;
};
RateControl rateControl;
}  // namespace

float gdxsv_audio_gain() {
	float gain = settings.aica.audioFade;
	if (0.f < settings.gdxsv.audioScale && settings.gdxsv.audioScale < 1.f) gain *= settings.gdxsv.audioScale;
	return gain;
}

void gdxsv_audio_write(s16 r, s16 l, void (*push)(s16 r, s16 l)) {
	RateControl& rc = rateControl;
	const Frame frame{l, r};
	if (rc.active != settings.gdxsv.audioRateControl) {
		rc = {};
		rc.active = settings.gdxsv.audioRateControl;
		rc.prev = frame;
	}
	if (!rc.active) {
		push(r, l);
		return;
	}

	// Linear interpolation between the previous input frame and this one.
	while (rc.phase < 1.0) {
		const float t = (float)rc.phase;
		push((s16)(rc.prev.r + (frame.r - rc.prev.r) * t), (s16)(rc.prev.l + (frame.l - rc.prev.l) * t));
		rc.phase += rc.step;
	}
	rc.phase -= 1.0;
	rc.prev = frame;
}

void gdxsv_audio_pushed(AudioBackend* backend) {
	RateControl& rc = rateControl;
	u32 queued, capacity;
	if (!rc.active || backend == nullptr || !backend->getQueueLevel(queued, capacity) || capacity == 0) return;
	rc.queueLevel += kQueueAlpha * (std::min<double>(1.0, (double)queued / capacity) - rc.queueLevel);
	rc.step = 1.0 / (1.0 + kRateMaxCorrection * (1.0 - 2.0 * rc.queueLevel));
}
