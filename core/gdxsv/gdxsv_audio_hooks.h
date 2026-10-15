// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "types.h"

class AudioBackend;

// Called by the audio stream (core/audio/audiostream.cpp).

// Extra output gain: replay fade-in and the 4-player replay volume.
float gdxsv_audio_gain();

// Writes one stereo frame through push, resampled while settings.gdxsv.audioRateControl is on (0 or more frames out).
void gdxsv_audio_write(s16 r, s16 l, void (*push)(s16 r, s16 l));

// After a buffer is pushed to the backend.
void gdxsv_audio_pushed(AudioBackend* backend);
