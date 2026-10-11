"""Verify the real warning hook, host toggle and savestate cleanup.

Run: python3 -m unittest discover -s gdxsv_patch/tests
Set GDX_DISC2_BIN to additionally verify the pinned ROM and original bytes.
"""
import hashlib
import io
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


PATCH = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PATCH))
import abi
import gen_abi


class WarningSoundTest(unittest.TestCase):
    def compile_and_run(self, source):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if compiler is None:
            self.skipTest("host C++ compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            binary = str(Path(directory) / "check")
            (Path(directory) / "types.h").write_text(
                '#pragma once\n#include <cstdint>\nusing u16 = uint16_t; using u32 = uint32_t;\n')
            gen_abi.write_host_header(Path(directory) / "gdxsv_patch_abi.h")
            subprocess.run([compiler, "-std=c++17", "-O2", "-DNDEBUG", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-parameter", "-pedantic-errors", "-I", directory,
                            "-I", str(PATCH.parent / "tests/inc"), "-include", "StandaloneCheck.h",
                            "-x", "c++", "-", "-o", binary],
                           input=source, text=True, check=True, capture_output=True)
            subprocess.run([binary], check=True)

    def test_volume_and_pan_forwarding(self):
        source = (PATCH / "src/main.c").read_text()
        function = re.search(r"void GDXSOUNDFUNC gdx_warning_sound_parameter\(.*?\n}",
                             source, re.S).group()
        function = function.replace("GDXSOUNDFUNC ", "").replace(
            "((void (*)(int, int, int, int))0x0c1a2650)", "parameter")
        expected = ", ".join(str(max(0, value - 16)) for value in range(128))
        self.compile_and_run(r'''
static int calls, received[4];
static void parameter(int port, int channel, int command, int value) {
    ++calls;
    received[0] = port; received[1] = channel;
    received[2] = command; received[3] = value;
}
''' + function + "\nstatic const int expected[] = {" + expected + r'''};
int main() {
    // Covers every possible stock 0..127 volume, including both gain tables
    // and all SFX options: e.g. 114 -> 98, 85 -> 69, 127 -> 111, 95 -> 79.
    for (int value = 0; value <= 127; ++value) {
        gdx_warning_sound_parameter(0, 12, 0x27b0, value);
        CHECK(calls == value + 1);
        CHECK(received[0] == 0 && received[1] == 12);
        CHECK(received[2] == 0x27b0 && received[3] == expected[value]);
    }
    // The four directional pans, including negative pan, pass unchanged.
    const int pans[] = {0, 0, -16, 16};
    for (int pan : pans) {
        gdx_warning_sound_parameter(0, 12, 0x2ab0, pan);
        CHECK(received[0] == 0 && received[1] == 12);
        CHECK(received[2] == 0x2ab0 && received[3] == pan);
    }
    gdx_warning_sound_parameter(3, 8, 0x1234, -127);
    CHECK(calls == 133);
    CHECK(received[0] == 3 && received[1] == 8);
    CHECK(received[2] == 0x1234 && received[3] == -127);
}
''')

    def host_harness(self, checks):
        # Compile the production toggle, restore callback and pre-save hook
        # against checked RAM and a cache-reset spy, using the generated ABI.
        core = PATCH.parent / "core/gdxsv"
        source = (core / "gdxsv.cpp").read_text()
        functions = [re.search(pattern, source, re.S).group() for pattern in (
            r"void Gdxsv::WriteWarningSoundMode\(.*?\n}",
            r"void Gdxsv::HookStateRestored\(.*?\n}",
            r"bool Gdxsv::IsSaveStateAllowed\(\) const \{.*?}",
            r"bool Gdxsv::IsReplaying\(\) const \{.*?}",
        )]
        functions.append(re.search(r"void gdxsv_emu_savestate\(.*?\n}",
                         (core / "gdxsv_emu_hooks.cpp").read_text(), re.S).group())
        self.compile_and_run(r'''
#include <map>
#include <string>
#include "gdxsv_patch_abi.h"
static int writes, resets, online_restores;
static std::map<u32, u32> memory;
static u32 gdxsv_ReadMem32(u32 addr) { return memory.at(addr); }
static bool gdxsv_WriteMem32(u32 addr, u32 value) {
    if (memory.at(addr) == value) return false;
    ++writes; memory[addr] = value; return true;
}
struct Emulator {
    Emulator* getSh4Executor() { return this; }
    void ResetCache() { ++resets; }
} emu;
enum class GdxsvMultiPovRole { None, Host, Guest };
static GdxsvMultiPovRole role = GdxsvMultiPovRole::None;
static GdxsvMultiPovRole gdxsv_multi_pov_current_role() { return role; }
struct Gdxsv {
    enum class NetMode { Offline, Lbs, McsUdp, McsRollback, Replay };
    NetMode netmode_ = NetMode::Offline;
    bool enabled_ = true;
    int disk_ = 2;
    std::map<std::string, u32> symbols_;
    bool Enabled() const { return enabled_; }
    bool IsSaveStateAllowed() const;
    bool IsReplaying() const;
    void RestoreOnlinePatch() { ++online_restores; }
    void InvalidateWidescreenPatch() {}
    void WriteWarningSoundMode(bool);
    void HookStateRestored();
} gdxsv;
''' + "\n".join(functions) + r'''
int main() {
    constexpr u32 site = 0x0c05ac14, stock = 0x0c1a2650, target = 0x0c4f05e4;
    constexpr u32 patch_addr = 0x0c4e0258, patch_id = 123456;
    memory[site] = stock;
    memory[patch_addr] = patch_id;
    for (const auto& guard : gdxsv_abi::warning_sound_guards)
        memory[guard.addr] = guard.stock;
    gdxsv.symbols_ = {{"gdx_entry_warning_sound_parameter", target},
                     {"patch_id", patch_addr}, {":patch_id", patch_id}};
''' + checks + "\n}\n")

    def test_host_installer(self):
        self.host_harness(r'''
    gdxsv.disk_ = 1;
    gdxsv.WriteWarningSoundMode(true);
    CHECK(writes == 0 && resets == 0 && memory[site] == stock);
    gdxsv.disk_ = 2;
    gdxsv.WriteWarningSoundMode(false);
    CHECK(writes == 0 && resets == 0);
    gdxsv.WriteWarningSoundMode(true);
    CHECK(writes == 1 && resets == 1 && memory[site] == target);
    gdxsv.WriteWarningSoundMode(true);
    CHECK(writes == 1 && resets == 1);
    gdxsv.WriteWarningSoundMode(false);
    CHECK(writes == 2 && resets == 2 && memory[site] == stock);
    gdxsv.WriteWarningSoundMode(false);
    CHECK(writes == 2 && resets == 2);

    for (bool enable : {false, true}) {
        const u32 original = enable ? stock : target;
        memory[site] = original;
        for (const auto& guard : gdxsv_abi::warning_sound_guards) {
            memory[guard.addr] ^= 1;
            gdxsv.WriteWarningSoundMode(enable);
            CHECK(memory[site] == original);
            memory[guard.addr] ^= 1;
        }
        memory[patch_addr] ^= 1;
        gdxsv.WriteWarningSoundMode(enable);
        CHECK(memory[site] == original);
        memory[patch_addr] ^= 1;
        for (const char* name : {"patch_id", ":patch_id", "gdx_entry_warning_sound_parameter"}) {
            const u32 value = gdxsv.symbols_.at(name);
            gdxsv.symbols_.erase(name);
            gdxsv.WriteWarningSoundMode(enable);
            CHECK(memory[site] == original && gdxsv.symbols_.count(name) == 0);
            gdxsv.symbols_[name] = value;
        }
        memory[site] = 0xdeadbeef;
        gdxsv.WriteWarningSoundMode(enable);
        CHECK(memory[site] == 0xdeadbeef);
    }
    CHECK(writes == 2 && resets == 2);
''')

    def test_savestate_lifecycle(self):
        self.host_harness(r'''
    using Mode = Gdxsv::NetMode;
    // Only four-screen Replay installs the hook after loading a state.
    for (auto mode : {Mode::Offline, Mode::Lbs, Mode::McsUdp, Mode::McsRollback, Mode::Replay}) {
        gdxsv.netmode_ = mode;
        for (auto view : {GdxsvMultiPovRole::None, GdxsvMultiPovRole::Host, GdxsvMultiPovRole::Guest}) {
            role = view;
            for (u32 saved : {stock, target}) {
                memory[site] = saved;
                gdxsv.HookStateRestored();
                const bool enabled = mode == Mode::Replay && view != GdxsvMultiPovRole::None;
                CHECK(memory[site] == (enabled ? target : stock));
            }
        }
    }
    // Replay/takeover and online modes reject disk saves; do not change the
    // active hook just because the pre-save callback ran before that check.
    for (auto mode : {Mode::Lbs, Mode::McsUdp, Mode::McsRollback, Mode::Replay}) {
        gdxsv.netmode_ = mode;
        memory[site] = target;
        CHECK(!gdxsv.IsSaveStateAllowed());
        gdxsv_emu_savestate(99);
        CHECK(memory[site] == target);
    }
    // Even if Stop was skipped, every permitted disk save restores stock.
    gdxsv.netmode_ = Mode::Offline;
    CHECK(gdxsv.IsSaveStateAllowed());
    for (int slot : {0, 90, 99}) {
        memory[site] = target;
        const int before = resets;
        gdxsv_emu_savestate(slot);
        CHECK(memory[site] == stock && resets == before + 1);
        gdxsv_emu_savestate(slot);
        CHECK(memory[site] == stock && resets == before + 1);
    }
    CHECK(online_restores == 10);
    // A different build's payload must be replaced before toggling its hooks.
    gdxsv.netmode_ = Mode::Replay;
    role = GdxsvMultiPovRole::Host;
    memory[patch_addr] ^= 1;
    gdxsv.HookStateRestored();
    CHECK(memory[site] == stock);
    memory[patch_addr] = patch_id;
    gdxsv.HookStateRestored();
    CHECK(memory[site] == target);
    gdxsv.WriteWarningSoundMode(false);
    CHECK(memory[site] == stock);
''')

    def test_payload_leaves_warning_site_stock(self):
        output = io.StringIO()
        gen_abi.emit_payload_hooks(output)
        hook, = [h for h in abi.HOOKS if h.feature == "warning_sound"]
        self.assertNotIn(f"0x{hook.addr:08x}", output.getvalue())

    def test_original_binary(self):
        path = os.environ.get("GDX_DISC2_BIN")
        if not path:
            self.skipTest("set GDX_DISC2_BIN to verify the pinned input")
        data = Path(path).read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(),
                         "2b12e9191a51bdeba7b1d6fd7da0399ba28cdbca479bc42af2cd7375caeca756")
        hook, = [h for h in abi.HOOKS if h.feature == "warning_sound"]
        for addr, expected in ((hook.addr, hook.stock), *hook.guards):
            self.assertEqual(struct.unpack_from("<I", data, addr - 0x0c010000)[0], expected)
        # Check every PC-relative mov.l that can reach this literal. Only the
        # warning wrapper's volume and pan calls may load the redirected target.
        loads = [pc for pc in range(hook.addr - 1024, hook.addr, 2)
                 if (op := struct.unpack_from("<H", data, pc - 0x0c010000)[0]) >> 12 == 0xD
                 and (pc & ~3) + 4 + (op & 0xFF) * 4 == hook.addr]
        self.assertEqual(loads, [0x0C05AB80, 0x0C05ABAC])


if __name__ == "__main__":
    unittest.main()
