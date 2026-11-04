from contextlib import redirect_stdout
from dataclasses import replace
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from desk_agent.agent import Agent, TaskState
from desk_agent.api import ConnectionCheck
from desk_agent.config import Config
from desk_agent.history import select_context
from desk_agent.setup_ui import configure
from desk_agent.tools import Tools


class RuntimeTests(unittest.TestCase):
    def test_cancelled_batch_is_repaired_and_next_task_can_run(self):
        class Client:
            def complete(self, messages, tools):
                if messages[-1]["role"] == "user" and messages[-1]["content"] == "next":
                    return {"role": "assistant", "content": "next done"}
                return {"role": "assistant", "tool_calls": [
                    {"id": "a", "function": {"name": "write_file", "arguments": '{"path":"x","content":"x","expected_sha256":null}'}},
                    {"id": "b", "function": {"name": "list_files", "arguments": '{"path":"."}'}}
                ]}
        with tempfile.TemporaryDirectory() as root:
            agent = Agent(Client(), Tools(root, lambda _: False), emit=lambda _: None)
            with self.assertRaises(RuntimeError):
                agent.run("cancel")
            self.assertEqual(agent.state, TaskState.CANCELLED)
            self.assertEqual([m["tool_call_id"] for m in agent.messages if m["role"] == "tool"], ["a", "b"])
            agent.run("next")
            self.assertEqual(agent.state, TaskState.FINISHED)

    def test_nonzero_shell_exit_does_not_accept_false_completion(self):
        class Client:
            def complete(self, messages, tools):
                if messages[-1]["role"] == "tool":
                    return {"role": "assistant", "content": "已经成功"}
                return {"role": "assistant", "tool_calls": [{"id": "a", "function": {"name": "run_shell", "arguments": '{"command":"exit 7"}'}}]}
        output = []
        with tempfile.TemporaryDirectory() as root:
            agent = Agent(Client(), Tools(root, lambda _: True, allow_shell=True), emit=output.append)
            with self.assertRaisesRegex(RuntimeError, "未成功"):
                agent.run("run")
            self.assertEqual(agent.state, TaskState.FAILED)
            self.assertNotIn("已经成功", output)

    def test_keyboard_interrupt_sets_cancelled_and_recovers(self):
        class Client:
            def complete(self, messages, tools):
                raise KeyboardInterrupt
        with tempfile.TemporaryDirectory() as root:
            agent = Agent(Client(), Tools(root, lambda _: True), emit=lambda _: None)
            with self.assertRaises(KeyboardInterrupt):
                agent.run("task")
            self.assertEqual(agent.state, TaskState.CANCELLED)

    def test_duplicate_call_ids_are_rejected_before_any_tool_executes(self):
        call = {"id": "same", "function": {"name": "write_file", "arguments": '{"path":"x","content":"x","expected_sha256":null}'}}
        class Client:
            def complete(self, messages, tools):
                return {"role": "assistant", "tool_calls": [call, call]}
        with tempfile.TemporaryDirectory() as root:
            agent = Agent(Client(), Tools(root, lambda _: True), emit=lambda _: None)
            with self.assertRaises(ValueError):
                agent.run("task")
            self.assertFalse((Path(root) / "x").exists())

    def test_context_drops_whole_turns_without_altering_full_record(self):
        messages = [{"role": "system", "content": "system"}]
        for i in range(9):
            messages.extend([{"role": "user", "content": str(i)},
                             {"role": "assistant", "tool_calls": [{"id": str(i)}]},
                             {"role": "tool", "tool_call_id": str(i), "content": "x" * 100},
                             {"role": "assistant", "content": "done"}])
        messages.append({"role": "user", "content": "current"})
        before = json.dumps(messages)
        selected, omitted = select_context(messages, [], max_chars=2000)
        self.assertGreater(omitted, 0)
        self.assertEqual(selected[0]["role"], "system")
        self.assertEqual(selected[1]["role"], "user")
        ids = {call["id"] for m in selected for call in m.get("tool_calls", [])}
        self.assertEqual(ids, {m["tool_call_id"] for m in selected if m["role"] == "tool"})
        self.assertEqual(json.dumps(messages), before)
        self.assertEqual(selected[-1]["content"], "current")

    def test_current_turn_over_budget_stops_before_request(self):
        class Client:
            def complete(self, *args):
                raise AssertionError("API must not be called")
        with tempfile.TemporaryDirectory() as root:
            agent = Agent(Client(), Tools(root, lambda _: True), emit=lambda _: None, max_context_chars=100)
            with self.assertRaisesRegex(RuntimeError, "预算"):
                agent.run("x" * 1000)
            self.assertEqual(agent.state, TaskState.FAILED)


class ConfigStagingTests(unittest.TestCase):
    def test_failed_connection_check_keeps_old_credentials_and_active_model(self):
        with tempfile.TemporaryDirectory() as root, patch.dict(os.environ, {"XDG_CONFIG_HOME": root}, clear=True):
            old = Config("https://old.example/v1", "old", "old-key")
            old.save()
            with patch("sys.stdin.isatty", return_value=True), patch("builtins.input", side_effect=["1", "https://new.example/v1", "new", "1"]), patch(
                "getpass.getpass", return_value="new-key"
            ), patch("desk_agent.api.ChatClient.check", return_value=ConnectionCheck(False, "工具调用不可用")), redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(RuntimeError, "原配置未改动"):
                    configure()
            self.assertEqual(Config.load(), old)

    def test_successful_check_activates_new_settings(self):
        with tempfile.TemporaryDirectory() as root, patch.dict(os.environ, {"XDG_CONFIG_HOME": root}, clear=True):
            old = Config("https://old.example/v1", "old", "old-key")
            old.save()
            with patch("sys.stdin.isatty", return_value=True), patch("builtins.input", side_effect=["1", "", "new", "1"]), patch(
                "getpass.getpass", return_value=""
            ), patch("desk_agent.api.ChatClient.check", return_value=ConnectionCheck(True, "测试通过")), redirect_stdout(io.StringIO()):
                configure()
            self.assertEqual(Config.load(), replace(old, model="new"))


if __name__ == "__main__":
    unittest.main()
