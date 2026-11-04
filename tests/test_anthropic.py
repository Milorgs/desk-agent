from contextlib import redirect_stdout
from dataclasses import replace
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch

from desk_agent.agent import Agent
from desk_agent.anthropic import payload, response
from desk_agent.api import ChatClient
from desk_agent.cli import configure, main
from desk_agent.config import Config
from desk_agent.tools import Tools


def reply(blocks, stop="end_turn"):
    return {"id": "msg_test", "type": "message", "role": "assistant", "content": blocks, "stop_reason": stop}


class NativeApiTests(unittest.TestCase):
    def setUp(self):
        self.responses, self.requests = [], []
        outer = self
        class Handler(BaseHTTPRequestHandler):
            def handle_request(self):
                raw = self.rfile.read(int(self.headers.get("Content-Length", 0)))
                outer.requests.append({"path": self.path, "body": json.loads(raw) if raw else None,
                                       "key": self.headers.get("x-api-key"),
                                       "version": self.headers.get("anthropic-version"),
                                       "authorization": self.headers.get("Authorization")})
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps(outer.responses.pop(0)).encode())
            do_GET = handle_request
            do_POST = handle_request
            def log_message(self, *args):
                pass
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.config = Config(f"http://127.0.0.1:{self.server.server_port}/v1/messages", "claude-test", "native-test-key", "claude", "anthropic")
        self.client = ChatClient(self.config)

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def test_tool_loop_groups_results_and_preserves_signed_blocks(self):
        blocks = [
            {"type": "thinking", "thinking": "private-thinking", "signature": "opaque-signature"},
            {"type": "text", "text": "正在创建并检查文件。"},
            {"type": "tool_use", "id": "toolu_write", "name": "write_file", "input": {
                "path": "hello.txt", "content": "你好 Claude", "expected_sha256": None}},
            {"type": "tool_use", "id": "toolu_read", "name": "read_file", "input": {"path": "hello.txt"}},
        ]
        self.responses = [reply(blocks, "tool_use"), reply([{"type": "text", "text": "文件已创建并核对。"}])]
        output = []
        with tempfile.TemporaryDirectory() as directory:
            agent = Agent(self.client, Tools(directory, lambda _: True), emit=output.append)
            agent.run("创建文件并检查")
            self.assertEqual((Path(directory) / "hello.txt").read_text(), "你好 Claude")
        first, second = self.requests
        self.assertEqual(first["path"], "/v1/messages")
        self.assertEqual(first["key"], "native-test-key")
        self.assertIsNone(first["authorization"])
        self.assertEqual(first["version"], "2023-06-01")
        self.assertEqual(first["body"]["max_tokens"], 4096)
        self.assertIn("input_schema", first["body"]["tools"][0])
        self.assertNotIn("function", first["body"]["tools"][0])
        self.assertIn("Linux CLI", first["body"]["system"])
        self.assertTrue(all(m["role"] != "system" for m in first["body"]["messages"]))
        self.assertEqual(second["body"]["messages"][-2]["content"], blocks)
        results = second["body"]["messages"][-1]
        self.assertEqual(results["role"], "user")
        self.assertEqual([r["tool_use_id"] for r in results["content"]], ["toolu_write", "toolu_read"])
        self.assertEqual(json.loads(results["content"][1]["content"])["content"], "你好 Claude")
        self.assertNotIn("private-thinking", "\n".join(output))

    def test_models_and_test_use_native_headers_and_auto_tool_choice(self):
        self.responses = [{"data": [{"id": "claude-test"}], "has_more": False},
                          reply([{"type": "tool_use", "id": "check", "name": "connection_check", "input": {}}], "tool_use")]
        self.assertEqual(self.client.models(), ["claude-test"])
        self.assertIn("连接成功", self.client.test())
        self.assertEqual(self.requests[0]["path"], "/v1/models?limit=1000")
        self.assertEqual(self.requests[1]["body"]["tool_choice"], {"type": "auto"})
        self.assertEqual(len(self.requests[1]["body"]["messages"]), 1)
        self.assertTrue(all(r["authorization"] is None for r in self.requests))

    def test_truncated_tool_response_does_not_write_files(self):
        self.responses = [reply([{"type": "tool_use", "id": "x", "name": "write_file", "input": {
            "path": "must-not-exist", "content": "partial", "expected_sha256": None}}], "max_tokens")]
        with tempfile.TemporaryDirectory() as directory:
            agent = Agent(self.client, Tools(directory, lambda _: True), emit=lambda _: None)
            with self.assertRaisesRegex(RuntimeError, "截断"):
                agent.run("任务")
            self.assertFalse((Path(directory) / "must-not-exist").exists())

    def test_tool_errors_are_marked_and_results_precede_followup_text(self):
        messages = [
            {"role": "system", "content": "system"},
            {"role": "user", "content": "task"},
            {"role": "assistant", "content": None, "tool_calls": [{"id": "x", "function": {"name": "read_file", "arguments": '{"path":"missing"}'}}]},
            {"role": "tool", "tool_call_id": "x", "content": '{"error":"cancelled"}'},
            {"role": "user", "content": "new task"},
        ]
        body = payload(self.config, messages, [], "auto")
        self.assertEqual([m["role"] for m in body["messages"]], ["user", "assistant", "user"])
        self.assertTrue(body["messages"][-1]["content"][0]["is_error"])
        self.assertEqual(body["messages"][-1]["content"][1]["type"], "text")
        self.assertNotIn("tools", body)

    def test_malformed_tool_input_or_duplicate_id_is_rejected(self):
        for blocks in [
            [{"type": "tool_use", "id": "x", "name": "read_file", "input": "invalid"}],
            [{"type": "tool_use", "id": "x", "name": "read_file", "input": {}}] * 2,
        ]:
            with self.subTest(blocks=blocks), self.assertRaises(RuntimeError):
                response(reply(blocks, "tool_use"))

    def test_refusal_and_unknown_stop_are_not_success(self):
        for stop in ["refusal", "pause_turn", None]:
            with self.subTest(stop=stop), self.assertRaises(RuntimeError):
                response(reply([{"type": "text", "text": "not complete"}], stop))


class NativeConfigTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        env = patch.dict(os.environ, {"XDG_CONFIG_HOME": str(self.root / "config")}, clear=True)
        env.start()
        self.addCleanup(env.stop)

    def test_native_settings_survive_other_provider_save_and_model_switch(self):
        config = Config("https://api.anthropic.com", "claude-test", "native-key", "claude", "anthropic", 8192, "2026-10-21")
        config.save()
        Config("https://other.example/v1", "other", "other-key", "other").save()
        Config.activate("claude")
        with patch("builtins.input", side_effect=["/model claude-next", "/exit"]), redirect_stdout(io.StringIO()):
            self.assertEqual(main(["--workspace", str(self.root)]), 0)
        loaded = Config.load()
        self.assertEqual((loaded.protocol, loaded.max_tokens, loaded.anthropic_version), ("anthropic", 8192, "2026-10-21"))
        self.assertEqual(loaded.model, "claude-next")
        self.assertEqual(loaded.base_url, "https://api.anthropic.com/v1")
        self.assertEqual(Config.load("other").protocol, "openai")

    def test_native_wizard_selects_correct_default_url(self):
        with patch("sys.stdin.isatty", return_value=True), patch("builtins.input", side_effect=["anthropic", "", "claude-test", "2"]), patch(
            "getpass.getpass", return_value="native-key"
        ), redirect_stdout(io.StringIO()):
            config = configure("claude", new=True)
        self.assertEqual(config.base_url, "https://api.anthropic.com/v1")
        self.assertEqual(Config.load().protocol, "anthropic")

    def test_native_endpoint_normalization_and_protocol_mismatch(self):
        for url in ["https://api.anthropic.com", "https://api.anthropic.com/v1/", "https://api.anthropic.com/v1/messages/"]:
            self.assertEqual(Config(url, "model", "key", protocol="anthropic").base_url, "https://api.anthropic.com/v1")
        with self.assertRaises(ValueError):
            Config("https://example.com/v1/messages", "model", "key").validate()
        with self.assertRaises(ValueError):
            Config("https://example.com/v1/chat/completions", "model", "key", protocol="anthropic").validate()

    def test_native_limits_are_validated(self):
        config = Config("https://api.anthropic.com/v1", "model", "key", protocol="anthropic")
        for value in [0, -1, True, "4096"]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                replace(config, max_tokens=value).validate()
        with self.assertRaises(ValueError):
            replace(config, anthropic_version="invalid\nheader").validate()


if __name__ == "__main__":
    unittest.main()
