import json
import urllib.error
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class ChatClient:
    """OpenAI-compatible Chat Completions transport; no agent framework."""

    def __init__(self, config):
        config.validate()
        self.config = config
        self.opener = urllib.request.build_opener(NoRedirect)

    def complete(self, messages, tools):
        body = json.dumps({
            "model": self.config.model, "messages": messages,
            "tools": tools, "tool_choice": "auto",
        }, ensure_ascii=False).encode()
        request = urllib.request.Request(
            self.config.base_url + "/chat/completions", data=body,
            headers={"Authorization": "Bearer " + self.config.api_key,
                     "Content-Type": "application/json"},
        )
        try:
            with self.opener.open(request, timeout=60) as response:
                raw = response.read(4_000_001)
            if len(raw) > 4_000_000:
                raise ValueError("API 响应过大。")
            message = json.loads(raw)["choices"][0]["message"]
            if not isinstance(message, dict):
                raise ValueError("API 返回了无效消息。")
            return message
        except urllib.error.HTTPError as exc:
            # Do not echo provider error bodies, which may include submitted content.
            raise RuntimeError(f"API 请求失败（HTTP {exc.code}），请检查地址、模型和密钥。") from None
        except (urllib.error.URLError, TimeoutError):
            raise RuntimeError("无法连接 API 或请求超时；未自动重试。") from None
        except (KeyError, IndexError, TypeError, json.JSONDecodeError):
            raise RuntimeError("API 响应格式不兼容，预期 Chat Completions 消息。") from None
