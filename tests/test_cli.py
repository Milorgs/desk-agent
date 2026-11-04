from contextlib import redirect_stdout, redirect_stderr
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from desk_agent.agent import Agent
from desk_agent.cli import main
from desk_agent.config import Config, auth_path, config_path
from desk_agent.tools import Tools


class CliTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.environment = patch.dict(os.environ, {"XDG_CONFIG_HOME": str(self.root / "config")}, clear=True)
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def test_config_roundtrip_private_key_and_environment_override(self):
        Config("https://example.com/v1", "my-model", "private-test-key").save()
        self.assertEqual(Config.load().api_key, "private-test-key")
        self.assertNotIn("private-test-key", config_path().read_text())
        self.assertEqual(auth_path().stat().st_mode & 0o777, 0o600)
        self.assertEqual(config_path().stat().st_mode & 0o777, 0o600)
        with patch.dict(os.environ, {"DESK_API_KEY": "override"}):
            self.assertEqual(Config.load().api_key, "override")
        tools = Tools(self.root, lambda _: True)
        self.assertIn("error", tools.call("read_file", {"path": str(auth_path())}))

    def test_first_run_wizard_saves_once_and_starts_chat(self):
        output = io.StringIO()
        with patch("sys.stdin.isatty", return_value=True), patch("builtins.input", side_effect=[
            "https://example.com/v1", "my-model", "/exit"
        ]), patch("getpass.getpass", return_value="private-test-key"), redirect_stdout(output):
            self.assertEqual(main(["--workspace", str(self.root)]), 0)
        self.assertEqual(Config.load().model, "my-model")
        self.assertNotIn("private-test-key", output.getvalue())
        with patch("builtins.input", side_effect=["/exit"]), patch("getpass.getpass") as ask_key, redirect_stdout(io.StringIO()):
            self.assertEqual(main(["--workspace", str(self.root)]), 0)
        ask_key.assert_not_called()

    def test_continuous_chat_keeps_context_and_new_clears_it(self):
        Config("https://example.com/v1", "my-model", "test-key").save()
        lengths = []
        def reply(client, messages, tools):
            lengths.append(len(messages))
            return {"role": "assistant", "content": "收到"}
        with patch("desk_agent.api.ChatClient.complete", autospec=True, side_effect=reply), patch(
            "builtins.input", side_effect=["第一个问题", "接着说", "/new", "新问题", "/exit"]
        ), redirect_stdout(io.StringIO()):
            self.assertEqual(main(["--workspace", str(self.root)]), 0)
        self.assertEqual(lengths, [2, 4, 2])

    def test_interrupt_does_not_exit_chat(self):
        Config("https://example.com/v1", "my-model", "test-key").save()
        with patch("desk_agent.api.ChatClient.complete", side_effect=[KeyboardInterrupt(), {"role": "assistant", "content": "第二次成功"}]), patch(
            "builtins.input", side_effect=["任务一", "任务二", "/exit"]
        ), redirect_stdout(io.StringIO()) as output:
            self.assertEqual(main(["--workspace", str(self.root)]), 0)
        self.assertIn("第二次成功", output.getvalue())

    def test_noninteractive_missing_config_does_not_hang(self):
        with patch("sys.stdin.isatty", return_value=False), redirect_stderr(io.StringIO()):
            self.assertEqual(main(["--workspace", str(self.root)]), 1)

    def test_denied_batch_repairs_pending_tool_calls(self):
        class Client:
            def complete(self, messages, tools):
                return {"role": "assistant", "tool_calls": [
                    {"id": "a", "function": {"name": "write_file", "arguments": json.dumps({"path": "x", "content": "x", "expected_sha256": None})}},
                    {"id": "b", "function": {"name": "list_files", "arguments": '{"path":"."}'}}
                ]}
        agent = Agent(Client(), Tools(self.root, lambda _: False), emit=lambda _: None)
        with self.assertRaises(RuntimeError):
            agent.run("写文件")
        self.assertEqual([m["tool_call_id"] for m in agent.messages if m["role"] == "tool"], ["a", "b"])

    def test_standalone_install_works_without_repo_cwd(self):
        source = Path(__file__).resolve().parents[1]
        prefix = self.root / "prefix with spaces"
        subprocess.run([sys.executable, str(source / "install.py"), "--prefix", str(prefix)], check=True, capture_output=True)
        result = subprocess.run([str(prefix / "bin/desk"), "--version"], cwd="/tmp", check=True, capture_output=True, text=True)
        self.assertEqual(result.stdout.strip(), "0.3.0")
        self.assertFalse((self.root / ".bashrc").exists())


if __name__ == "__main__":
    unittest.main()
