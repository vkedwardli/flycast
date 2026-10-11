"""Compile the real audio stream with a fake device and frame transport.

Run: python3 -m unittest discover -s tests/audio_stream -v
The device stays empty to exercise adaptive resampling without audio hardware.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ReplayOutputTest(unittest.TestCase):
    def test_mixed_output_uses_resampler_and_silent_frames_use_transport(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")
        stubs = {
            "types.h": r'''
#pragma once
#include <cstdint>
#include <string>
using u8 = uint8_t; using u32 = uint32_t; using s16 = int16_t;
#define INFO_LOG(...) ((void)0)
#define WARN_LOG(...) ((void)0)
''',
            "cfg/option.h": r'''
#pragma once
namespace config {
inline bool LimitFPS = false;
inline std::string AudioBackend = "test";
struct Volume { float dbPower() const { return 1.f; } };
inline Volume AudioVolume;
}
''',
            "emulator.h": r'''
#pragma once
#include <functional>
struct Settings {
    struct { float audioFade = 1.f; } aica;
    struct { float audioScale = 1.f; bool audioRateControl = true; bool replayModeActive = true; } gdxsv;
};
inline Settings settings;
enum class Event { Terminate, LoadState };
struct EventManager {
    inline static std::function<void(Event, void*)> callbacks[2];
    template<class F> static void listen(Event event, F callback) { callbacks[int(event)] = callback; }
    static void event(Event event) { callbacks[int(event)](event, nullptr); }
};
''',
            "gdxsv/gdxsv_multi_pov.h": r'''
#pragma once
#include "gdxsv/gdxsv_multi_pov_audio.h"
enum class GdxsvMultiPovRole { None, Host, Guest };
GdxsvMultiPovRole gdxsv_multi_pov_current_role();
int gdxsv_multi_pov_guest_pov();
void gdxsv_multi_pov_audio_enable(bool);
void gdxsv_multi_pov_audio_reset();
uint32_t gdxsv_multi_pov_audio_frame(int64_t, uint32_t,
    const GdxsvMultiPovAudioSample*, uint32_t, GdxsvMultiPovAudioSample*, uint32_t = 0);
''',
        }
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            for path, contents in stubs.items():
                target = tmp / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(contents)
            binary = tmp / "check"
            subprocess.run([compiler, "-std=c++17", "-O2", "-DNDEBUG", "-I", str(tmp),
                            "-I", str(ROOT / "core"),
                            str(Path(__file__).with_name("replay_output.cpp")),
                            "-o", str(binary)], check=True, capture_output=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
