"""Exercise the real SDL output callback without an audio device."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SdlRecoveryTest(unittest.TestCase):
    def test_seek_and_underrun_reprime_pcm_without_changing_other_output(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")
        stubs = {
            "types.h": r'''
#pragma once
#include <cstdint>
using u8 = uint8_t; using u32 = uint32_t; using s16 = int16_t;
#define INFO_LOG(...) ((void)0)
#define ERROR_LOG(...) ((void)0)
#define DEBUG_LOG(...) ((void)0)
''',
            "cfg/option.h": r'''
#pragma once
namespace config { inline u32 AudioBufferSize = 2822; }
''',
            "stdclass.h": r'''
#pragma once
#include <cmath>
struct cResetEvent { void Set() {} void Wait() {} };
''',
            "SDL.h": r'''
#pragma once
#include <cstdint>
using Uint8 = uint8_t; using SDL_AudioDeviceID = uint32_t;
constexpr int SDL_INIT_AUDIO = 16, AUDIO_S16 = 32784;
constexpr int SDL_AUDIO_PAUSED = 2, SDL_AUDIO_PLAYING = 1;
struct SDL_AudioSpec {
    int freq; uint16_t format; uint8_t channels; uint16_t samples;
    void (*callback)(void*, Uint8*, int); void* userdata;
};
struct SDL_AudioCVT {
    double len_ratio = 1; int len_mult = 1, len = 0, len_cvt = 0, needed = 1;
    Uint8* buf = nullptr;
};
inline SDL_AudioSpec outputSpec;
inline int deviceStatus = SDL_AUDIO_PAUSED;
inline int SDL_WasInit(int value) { return value; }
inline int SDL_InitSubSystem(int) { return 0; }
inline const char* SDL_GetError() { return ""; }
inline SDL_AudioDeviceID SDL_OpenAudioDevice(const char*, int, SDL_AudioSpec* in, SDL_AudioSpec* out, int) {
    outputSpec = *out = *in; deviceStatus = SDL_AUDIO_PAUSED; return 1;
}
inline int SDL_GetAudioDeviceStatus(SDL_AudioDeviceID) { return deviceStatus; }
inline void SDL_PauseAudioDevice(SDL_AudioDeviceID, int paused) { deviceStatus = paused ? SDL_AUDIO_PAUSED : SDL_AUDIO_PLAYING; }
inline void SDL_CloseAudioDevice(SDL_AudioDeviceID) {}
inline int SDL_BuildAudioCVT(SDL_AudioCVT*, int, int, int, int, int, int) { return 1; }
inline void SDL_ConvertAudio(SDL_AudioCVT*) {}
''',
        }
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            for path, contents in stubs.items():
                target = tmp / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(contents)
            binary = tmp / "check"
            result = subprocess.run([compiler, "-std=c++17", "-O2", "-DNDEBUG", "-I", str(tmp),
                                     "-I", str(ROOT / "core"),
                                     str(Path(__file__).with_name("sdl_recovery.cpp")),
                                     "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
