import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import tempfile
import threading
import unittest

from desk_agent.agent import Agent
from desk_agent.api import ChatClient
from desk_agent.config import Config
from desk_agent.tools import Tools


class ToolTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.tools = Tools(self.root, lambda _: True)

    def test_write_read_backup_and_stale_edit(self):
        self.assertNotIn("error", self.tools.call("write_file", {"path": "你好.txt", "content": "第一版", "expected_sha256": None}))
        original = self.tools.call("read_file", {"path": "你好.txt"})
        self.assertEqual(original["content"], "第一版")
        result = self.tools.call("write_file", {"path": "你好.txt", "content": "第二版", "expected_sha256": original["sha256"]})
        self.assertEqual((self.root / result["backup"]).read_text(), "第一版")
        stale = self.tools.call("write_file", {"path": "你好.txt", "content": "错误覆盖", "expected_sha256": original["sha256"]})
        self.assertIn("error", stale)
        self.assertEqual((self.root / "你好.txt").read_text(), "第二版")

    def test_path_escape_and_symlink(self):
        self.assertIn("error", self.tools.call("read_file", {"path": "../outside"}))
        (self.root / "escape").symlink_to("/etc")
        self.assertIn("error", self.tools.call("read_file", {"path": "escape/passwd"}))
        self.assertIn("error", self.tools.call("read_file", {"path": ".env"}))

    def test_denial_and_unknown_arguments(self):
        self.tools.approve = lambda _: False
        result = self.tools.call("write_file", {"path": "no.txt", "content": "no", "expected_sha256": None})
        self.assertTrue(result["denied"])
        self.assertFalse((self.root / "no.txt").exists())
        self.assertIn("error", self.tools.call("run_shell", {"command": "true"}))
        self.assertIn("error", self.tools.call("read_file", {"path": "x", "extra": "x"}))

    def test_shell_result_and_timeout(self):
        tools = Tools(self.root, lambda _: True, allow_shell=True)
        result = tools.call("run_shell", {"command": "printf 'hello'; exit 7"})
        self.assertEqual(result["output"], "hello")
        self.assertEqual(result["exit_code"], 7)
        tools.shell_timeout = 0.1
        result = tools.call("run_shell", {"command": "sleep 30"})
        self.assertTrue(result["timed_out"])
        self.assertLess(result["exit_code"], 0)

    def test_large_and_binary_files(self):
        (self.root / "large").write_bytes(b"a" * 65537)
        (self.root / "binary").write_bytes(b"\xff\xfe")
        for name in ["large", "binary"]:
            self.assertIn("error", self.tools.call("read_file", {"path": name}))

    def test_change_during_approval(self):
        target = self.root / "file"
        target.write_text("before")
        digest = hashlib.sha256(b"before").hexdigest()
        def approve(_):
            target.write_text("human edit")
            return True
        self.tools.approve = approve
        result = self.tools.call("write_file", {"path": "file", "content": "AI edit", "expected_sha256": digest})
        self.assertIn("error", result)
        self.assertEqual(target.read_text(), "human edit")


class IntegrationTests(unittest.TestCase):
    def test_http_api_tool_loop_creates_and_verifies_file(self):
        """Real local HTTP transport and tools; explicitly NOT a live model test."""
        requests = []
        replies = [
            {"role": "assistant", "content": None, "tool_calls": [{"id": "one", "type": "function", "function": {
                "name": "write_file", "arguments": json.dumps({"path": "hello.txt", "content": "你好 Linux", "expected_sha256": None})}}]},
            {"role": "assistant", "content": None, "tool_calls": [{"id": "two", "type": "function", "function": {
                "name": "read_file", "arguments": '{"path":"hello.txt"}'}}]},
            {"role": "assistant", "content": "已写入并读取核对。"},
        ]
        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                requests.append(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
                message = replies[len(requests) - 1]
                body = json.dumps({"choices": [{"message": message}]}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory:
                config = Config(f"http://127.0.0.1:{server.server_port}/v1", "test-model", "test-key")
                agent = Agent(ChatClient(config), Tools(directory, lambda _: True), emit=lambda _: None)
                agent.run("创建 hello.txt 并验证")
                self.assertEqual((Path(directory) / "hello.txt").read_text(), "你好 Linux")
                self.assertEqual(len(requests), 3)
                tool_result = requests[2]["messages"][-1]
                self.assertEqual(json.loads(tool_result["content"])["content"], "你好 Linux")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_step_limit_and_denial_stop(self):
        class Repeating:
            def complete(self, messages, tools):
                return {"role": "assistant", "tool_calls": [{"id": "repeat", "type": "function", "function": {
                    "name": "list_files", "arguments": '{"path":"."}'}}]}
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, "上限"):
                Agent(Repeating(), Tools(directory, lambda _: True), emit=lambda _: None, max_steps=2).run("任务")

    def test_reject_insecure_remote_api(self):
        with self.assertRaisesRegex(ValueError, "HTTPS"):
            Config("http://example.com/v1", "test", "secret").validate()


if __name__ == "__main__":
    unittest.main()
