"""Run the real video wait against a guest blocked by a full PCM queue."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(hasattr(os, "fork"), "requires POSIX shared memory and fork")
class SyncRecoveryTest(unittest.TestCase):
    def test_video_wait_drains_pcm_so_a_guest_can_reach_the_host(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            (tmp / "log").mkdir()
            (tmp / "log/LogManager.h").write_text("#pragma once\n")
            (tmp / "types.h").write_text("""
#pragma once
#define NOTICE_LOG(...) ((void)0)
#define WARN_LOG(...) ((void)0)
""")
            binary = tmp / "check"
            result = subprocess.run([compiler, "-std=c++17", "-O2", "-DNDEBUG", "-pthread",
                                     "-I", str(tmp), "-I", str(ROOT / "core"),
                                     str(Path(__file__).with_name("sync_recovery.cpp")),
                                     "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
