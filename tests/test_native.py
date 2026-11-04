import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch

from native.build import build
from desk_agent.process import run_c, run_python
from desk_agent.tools import Tools


def alive(pid):
    try:
        state = Path(f"/proc/{pid}/stat").read_text().split(")", 1)[1].split()[0]
        return state not in {"Z", "X"}
    except FileNotFoundError:
        return False


@unittest.skipUnless(shutil.which("cc"), "C compiler is not installed")
class NativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_dir = tempfile.TemporaryDirectory()
        cls.binary = build(Path(cls.build_dir.name) / "desk-runner")

    @classmethod
    def tearDownClass(cls):
        cls.build_dir.cleanup()

    def test_native_and_python_results_match(self):
        commands = ["printf '你好'; printf ' error' >&2; exit 7", "pwd", "printf '\\377'", "head -c 17000 /dev/zero"]
        with tempfile.TemporaryDirectory() as root:
            for command in commands:
                with self.subTest(command=command):
                    self.assertEqual(run_c(root, command, 3, self.binary), run_python(root, command, 3))

    def test_native_timeout_kills_child_process_group(self):
        with tempfile.TemporaryDirectory() as root:
            result = run_c(root, "sleep 30 & echo $! > child.pid; wait", 0.1, self.binary)
            self.assertTrue(result["timed_out"])
            self.assertLess(result["exit_code"], 0)
            pid = int((Path(root) / "child.pid").read_text())
            deadline = time.monotonic() + 2
            while alive(pid) and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertFalse(alive(pid))

    def test_native_termination_cleans_up_before_exiting(self):
        with tempfile.TemporaryDirectory() as root:
            process = subprocess.Popen([str(self.binary), root, "30000", "sleep 30 & echo $! > child.pid; wait"],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
            try:
                deadline = time.monotonic() + 2
                path = Path(root) / "child.pid"
                while not path.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(path.exists())
                pid = int(path.read_text())
                process.terminate()
                process.communicate(timeout=3)
                self.assertEqual(process.returncode, 143)
                deadline = time.monotonic() + 2
                while alive(pid) and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertFalse(alive(pid))
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()

    def test_credentials_are_filtered_for_both_backends(self):
        with tempfile.TemporaryDirectory() as root, patch.dict(os.environ, {"DESK_API_KEY": "private-test-key"}):
            command = "printf '%s' \"${DESK_API_KEY-unset}\""
            self.assertEqual(run_c(root, command, 3, self.binary)["output"], "unset")
            self.assertEqual(run_python(root, command, 3)["output"], "unset")

    def test_native_failure_never_falls_back_and_repeats_command(self):
        with tempfile.TemporaryDirectory() as root:
            fake = Path(root) / "broken-runner"
            fake.write_text("#!/bin/sh\nexit 2\n")
            fake.chmod(0o700)
            with self.assertRaisesRegex(RuntimeError, "未重新执行"):
                run_c(root, "touch must-not-exist", 1, fake)
            self.assertFalse((Path(root) / "must-not-exist").exists())

    def test_tools_use_selected_c_backend_and_still_require_approval(self):
        with tempfile.TemporaryDirectory() as root, patch("desk_agent.process.native_path", return_value=self.binary):
            tools = Tools(root, lambda _: True, allow_shell=True, shell_backend="c")
            self.assertEqual(tools.call("run_shell", {"command": "printf 'C backend'"})["output"], "C backend")
            tools.approve = lambda _: False
            self.assertTrue(tools.call("run_shell", {"command": "touch denied"})["denied"])
            self.assertFalse((Path(root) / "denied").exists())

    def test_native_install_contains_working_runner(self):
        import sys
        source = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as root:
            prefix = Path(root) / "local"
            subprocess.run([sys.executable, str(source / "install.py"), "--prefix", str(prefix), "--with-native"], check=True, capture_output=True)
            binary = next((prefix / "share/desk-agent/releases").glob("*/desk_agent/_native/desk-runner"))
            self.assertEqual(run_c(root, "printf 'installed'", 3, binary)["output"], "installed")


if __name__ == "__main__":
    unittest.main()
