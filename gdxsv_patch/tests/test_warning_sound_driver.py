"""Audio-scale regression test against the user's pinned disc-2 assets.

Set GDX_DISC2_AICA and GDX_DISC2_SOUND_BANK to enable this test. No game
assets are included. It checks driver total level, without rendering audio.
"""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

PATCH = Path(__file__).resolve().parents[1]
TYPES = '#pragma once\n#include <algorithm>\n#include <array>\n#include <cstdint>\n#include <cstdio>\n#include <cstring>\n#include <cstdlib>\n#include <string>\n#include <vector>\n#include <cassert>\nusing u8=uint8_t; using s8=int8_t; using u16=uint16_t; using s16=int16_t; using u32=uint32_t; using s32=int32_t; using u64=uint64_t; using s64=int64_t;\n#define FEAT_AREC 0\n#define DYNAREC_NONE 0\n#define DYNACALL\n#define AICA_ARM_CLOCK 22579200\n#define ARAM_MASK 0x1fffff\n#define likely(x) (x)\n#define unlikely(x) (x)\n#define INFO_LOG(...) ((void)0)\n#define DEBUG_LOG(...) ((void)0)\n#define WARN_LOG(...) ((void)0)\n#define ERROR_LOG(...) ((void)0)\n#define verify(x) assert(x)\n#define die(x) abort()\nconstexpr u32 operator ""_MB(unsigned long long x) { return x*1024*1024; }\nstruct OnLoad { OnLoad(void (*f)()) { f(); } };\n'
AICA = '#pragma once\n#include "types.h"\nnamespace aica { extern std::array<u8, 2*1024*1024> aica_ram; void timeStep(); }\n'


class WarningSoundDriverTest(unittest.TestCase):
    def compile_driver(self, directory):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if compiler is None:
            self.skipTest("host C++ compiler unavailable")
        hook = re.search(r"void GDXSOUNDFUNC gdx_warning_sound_parameter\(.*?\n}",
                         (PATCH / "src/main.c").read_text(), re.S).group()
        hook = hook.replace("GDXSOUNDFUNC ", "").replace(
            "((void (*)(int, int, int, int))0x0c1a2650)", "parameter")
        source = (PATCH / "tests/warning_sound_driver.cpp").read_text().replace(
            "// WARNING_HOOK", hook)
        tmp = Path(directory)
        (tmp / "hw/aica").mkdir(parents=True)
        (tmp / "types.h").write_text(TYPES)
        (tmp / "hw/aica/aica_if.h").write_text(AICA)
        (tmp / "check.cpp").write_text(source)
        binary = tmp / "check"
        subprocess.run([compiler, "-std=c++17", "-O2", "-DNDEBUG", "-I", str(tmp),
                        "-I", str(PATCH.parent / "tests/inc"),
                        "-I", str(PATCH.parent / "core"), str(tmp / "check.cpp"),
                        "-o", str(binary)], check=True, capture_output=True)
        return binary

    def test_release_driver_build(self):
        # Keep the Release harness buildable even without proprietary assets.
        with tempfile.TemporaryDirectory() as directory:
            self.compile_driver(directory)

    def test_driver_amplitude(self):
        driver = os.environ.get("GDX_DISC2_AICA")
        bank = os.environ.get("GDX_DISC2_SOUND_BANK")
        if not driver or not bank:
            self.skipTest("set GDX_DISC2_AICA and GDX_DISC2_SOUND_BANK")
        for path, digest in (
            (driver, "b775b13ab0df29a93cf98f8aff3beb1b7637c1952018dab706e2b3ad4d5f9887"),
            (bank, "8ecbd7e84b7e704d4e74382eee8976ed989f29ff57df95ea76fa04cf47f82701"),
        ):
            self.assertEqual(hashlib.sha256(Path(path).read_bytes()).hexdigest(), digest)
        with tempfile.TemporaryDirectory() as directory:
            binary = self.compile_driver(directory)
            subprocess.run([str(binary), driver, bank], check=True)
