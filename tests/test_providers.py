from contextlib import redirect_stdout, redirect_stderr
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch

from desk_agent.api import ChatClient
from desk_agent.cli import configure, main
from desk_agent.config import Config, auth_path, config_path


class ProviderTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        env = patch.dict(os.environ, {"XDG_CONFIG_HOME": str(self.root / "config")}, clear=True)
        env.start()
        self.addCleanup(env.stop)

    def test_profiles_keep_independent_models_and_keys(self):
        Config("https://one.example/v1", "model-one", "key-one", "one").save()
        Config("https://two.example/v1", "model-two", "key-two", "two").save()
        self.assertEqual(Config.load().profile, "two")
        one = Config.activate("one")
        self.assertEqual((one.api_key, one.model), ("key-one", "model-one"))
        self.assertEqual(Config.load().profile, "one")
        self.assertEqual(Config.load("two").api_key, "key-two")
        self.assertNotIn("key-one", config_path().read_text())

    def test_legacy_configuration_survives_adding_provider(self):
        config_path().parent.mkdir(parents=True)
        config_path().write_text('[model]\nbase_url="https://legacy.example/v1"\nname="old-model"\n')
        auth_path().write_text('{"api_key":"old-key"}')
        self.assertEqual(Config.load().api_key, "old-key")
        Config("https://new.example/v1", "new-model", "new-key", "new").save()
        self.assertEqual(Config.activate("default").api_key, "old-key")
        self.assertEqual(Config.load("new").model, "new-model")
        self.assertEqual(auth_path().stat().st_mode & 0o777, 0o600)

    def test_domain_change_requires_new_key_without_losing_existing_config(self):
        Config("https://one.example/v1", "model-one", "key-one").save()
        with patch("sys.stdin.isatty", return_value=True), patch("builtins.input", side_effect=["", "https://two.example/v1", "model-two"]), patch(
            "getpass.getpass", return_value=""
        ), redirect_stdout(io.StringIO()), self.assertRaises(ValueError):
            configure()
        self.assertEqual(Config.load().api_key, "key-one")
        self.assertEqual(Config.load().base_url, "https://one.example/v1")

    def test_switch_provider_resets_history_and_uses_its_key(self):
        Config("https://one.example/v1", "model-one", "key-one", "one").save()
        Config("https://two.example/v1", "model-two", "key-two", "two").save()
        Config.activate("one")
        observed = []
        def complete(client, messages, tools):
            observed.append((client.config.profile, client.config.api_key, [m["content"] for m in messages]))
            return {"role": "assistant", "content": "完成"}
        with patch("desk_agent.api.ChatClient.complete", autospec=True, side_effect=complete), patch(
            "builtins.input", side_effect=["第一家的私有任务", "/provider two", "第二家的任务", "/exit"]
        ), redirect_stdout(io.StringIO()):
            self.assertEqual(main(["--workspace", str(self.root)]), 0)
        self.assertEqual(observed[1][0:2], ("two", "key-two"))
        self.assertNotIn("第一家的私有任务", observed[1][2])
        self.assertEqual(Config.load().profile, "two")

    def test_model_switch_is_saved_only_in_current_profile(self):
        Config("https://one.example/v1", "old", "key-one", "one").save()
        Config("https://two.example/v1", "other", "key-two", "two").save()
        Config.activate("one")
        with patch("builtins.input", side_effect=["/model newer", "/exit"]), redirect_stdout(io.StringIO()):
            self.assertEqual(main(["--workspace", str(self.root)]), 0)
        self.assertEqual(Config.load("one").model, "newer")
        self.assertEqual(Config.load("two").model, "other")

    def test_explicit_provider_ignores_environment_credentials(self):
        Config("https://one.example/v1", "model-one", "key-one", "one").save()
        with patch.dict(os.environ, {"DESK_API_KEY": "wrong-env-key"}):
            self.assertEqual(Config.load("one", use_env=False).api_key, "key-one")


class ApiTests(unittest.TestCase):
    def setUp(self):
        self.responses = []
        self.requests = []
        outer = self
        class Handler(BaseHTTPRequestHandler):
            def handle_request(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
                outer.requests.append({"path": self.path, "body": json.loads(body) if body else None,
                                       "auth": self.headers.get("Authorization")})
                status, data = outer.responses.pop(0)
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps(data).encode())
            do_POST = handle_request
            do_GET = handle_request
            def log_message(self, *args):
                pass
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.client = ChatClient(Config(f"http://127.0.0.1:{self.server.server_port}/v1/chat/completions/", "model", "test-key"))

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def test_models_and_connection_test_use_minimal_requests(self):
        self.responses = [(200, {"data": [{"id": "b"}, {"id": "a"}, {"id": "a"}]}),
                          (200, {"choices": [{"message": {"role": "assistant", "tool_calls": [{"id": "test", "function": {
                              "name": "connection_check", "arguments": "{}"}}]}}]})]
        self.assertEqual(self.client.models(), ["a", "b"])
        self.assertIn("连接成功", self.client.test())
        self.assertEqual([r["path"] for r in self.requests], ["/v1/models", "/v1/chat/completions"])
        self.assertEqual(len(self.requests[1]["body"]["messages"]), 1)
        self.assertEqual(self.requests[1]["auth"], "Bearer test-key")

    def test_transient_failure_retries_only_api_request(self):
        self.responses = [(503, {"error": {"message": "private-detail"}}), (200, {"choices": [{"message": {"content": "ok"}}]})]
        with patch("desk_agent.api.time.sleep") as sleep, redirect_stderr(io.StringIO()):
            self.assertEqual(self.client.complete([], [])["content"], "ok")
        self.assertEqual(len(self.requests), 2)
        sleep.assert_called_once()

    def test_auth_failure_is_clear_and_not_retried_or_leaked(self):
        self.responses = [(401, {"error": {"message": "test-key private-detail"}})]
        with self.assertRaises(RuntimeError) as caught:
            self.client.complete([], [])
        self.assertIn("密钥", str(caught.exception))
        self.assertNotIn("test-key", str(caught.exception))
        self.assertEqual(len(self.requests), 1)

    def test_timeout_not_retried(self):
        with patch.object(self.client.opener, "open", side_effect=TimeoutError()) as request, self.assertRaisesRegex(RuntimeError, "超时"):
            self.client.complete([], [])
        self.assertEqual(request.call_count, 1)

    def test_truncated_tool_response_is_not_executed(self):
        self.responses = [(200, {"choices": [{"finish_reason": "length", "message": {"content": "partial"}}]})]
        with self.assertRaisesRegex(RuntimeError, "截断"):
            self.client.complete([], [])


if __name__ == "__main__":
    unittest.main()
