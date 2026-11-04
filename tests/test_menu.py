from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from desk_agent.cli import main
from desk_agent.config import Config


class MenuTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        env = patch.dict(os.environ, {"XDG_CONFIG_HOME": str(self.root / "config")}, clear=True)
        env.start()
        self.addCleanup(env.stop)

    def run_menu(self, inputs):
        output = io.StringIO()
        with patch("sys.stdin.isatty", return_value=True), patch("builtins.input", side_effect=inputs), redirect_stdout(output):
            result = main(["--workspace", str(self.root)])
        self.assertEqual(result, 0)
        return output.getvalue()

    def save_two(self):
        Config("https://one.example/v1", "one", "key-one", "one").save()
        Config("https://two.example/v1", "two", "key-two", "two").save()

    def test_fresh_install_can_exit_without_configuration_or_network(self):
        with patch("desk_agent.api.ChatClient.request") as request, patch("getpass.getpass") as key:
            output = self.run_menu(["0"])
        self.assertIn("[1] 开始对话", output)
        self.assertIn("[2] 选择模型", output)
        self.assertIn("[3] 添加模型", output)
        self.assertFalse(Config.profiles()[1])
        request.assert_not_called()
        key.assert_not_called()

    def test_start_or_select_without_models_returns_to_menu(self):
        with patch("desk_agent.api.ChatClient.request") as request:
            output = self.run_menu(["1", "2", "0"])
        self.assertIn("请先选择 [3] 添加模型", output)
        request.assert_not_called()

    def test_numbered_selection_changes_provider_used_for_chat(self):
        self.save_two()
        observed = []
        def complete(client, messages, tools):
            observed.append((client.config.profile, client.config.api_key))
            return {"role": "assistant", "content": "完成"}
        with patch("desk_agent.api.ChatClient.complete", autospec=True, side_effect=complete):
            self.run_menu(["2", "1", "1", "测试", "/exit"])
        self.assertEqual(observed, [("one", "key-one")])
        self.assertEqual(Config.load().profile, "one")

    def test_add_accepts_chinese_name_and_numbered_anthropic_protocol(self):
        with patch("getpass.getpass", return_value="native-key"), patch("desk_agent.api.ChatClient.request") as request:
            output = self.run_menu(["3", "2", "日常助手", "", "claude-test", "2", "0"])
        config = Config.load()
        self.assertEqual((config.profile, config.protocol), ("日常助手", "anthropic"))
        self.assertEqual(config.base_url, "https://api.anthropic.com/v1")
        self.assertNotIn("native-key", output)
        request.assert_not_called()

    def test_cancel_and_invalid_selection_preserve_current_model(self):
        self.save_two()
        self.run_menu(["invalid", "2", "999", "0", "3", "0", "0"])
        self.assertEqual(Config.load().profile, "two")
        self.assertEqual(len(Config.profiles()[1]), 2)

    def test_return_from_chat_keeps_context_until_model_changes(self):
        self.save_two()
        observed = []
        def complete(client, messages, tools):
            observed.append((client.config.profile, len(messages)))
            return {"role": "assistant", "content": "完成"}
        with patch("desk_agent.api.ChatClient.complete", autospec=True, side_effect=complete):
            self.run_menu(["1", "问题一", "/menu", "1", "追问", "/menu", "2", "1", "1", "新模型问题", "/exit"])
        self.assertEqual(observed, [("two", 2), ("two", 4), ("one", 2)])

    def test_interrupt_add_does_not_create_partial_configuration(self):
        self.run_menu(["3", KeyboardInterrupt(), "0"])
        self.assertFalse(Config.profiles()[1])

    def test_protocol_exit_cancels_before_name_and_key_and_preserves_selection(self):
        self.save_two()
        with patch("getpass.getpass") as key, patch("desk_agent.api.ChatClient.request") as request:
            self.run_menu(["3", "0", "0"])
        self.assertEqual(Config.load().profile, "two")
        self.assertEqual(len(Config.profiles()[1]), 2)
        key.assert_not_called()
        request.assert_not_called()

    def test_cancel_config_keeps_current_chat_context(self):
        self.save_two()
        observed = []
        def complete(client, messages, tools):
            observed.append((client.config.profile, len(messages)))
            return {"role": "assistant", "content": "完成"}
        with patch("getpass.getpass") as key, patch("desk_agent.api.ChatClient.complete", autospec=True, side_effect=complete):
            self.run_menu(["1", "问题一", "/config", "0", "追问", "/exit"])
        self.assertEqual(observed, [("two", 2), ("two", 4)])
        key.assert_not_called()

    def test_cancel_first_run_single_task_configuration_exits_cleanly(self):
        with patch("sys.stdin.isatty", return_value=True), patch("builtins.input", side_effect=["0"]), patch(
            "desk_agent.api.ChatClient.request"
        ) as request, redirect_stdout(io.StringIO()):
            self.assertEqual(main(["--workspace", str(self.root), "一次性任务"]), 0)
        self.assertFalse(Config.profiles()[1])
        request.assert_not_called()

    def test_existing_model_can_start_with_enter(self):
        Config("https://example.com/v1", "model", "key").save()
        with patch("getpass.getpass") as key:
            self.run_menu(["", "/exit"])
        key.assert_not_called()

    def test_single_task_bypasses_menu_even_in_interactive_terminal(self):
        Config("https://example.com/v1", "model", "key").save()
        with patch("sys.stdin.isatty", return_value=True), patch("builtins.input") as prompt, patch(
            "desk_agent.api.ChatClient.complete", return_value={"role": "assistant", "content": "完成"}
        ), redirect_stdout(io.StringIO()):
            self.assertEqual(main(["--workspace", str(self.root), "一次性任务"]), 0)
        prompt.assert_not_called()


if __name__ == "__main__":
    unittest.main()
