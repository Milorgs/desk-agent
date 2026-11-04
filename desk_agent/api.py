import json
import sys
import time
import urllib.error
import urllib.request

from . import anthropic


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


ERRORS = {
    400: "请求格式或模型参数不兼容；请检查接口类型、模型及工具调用支持。",
    401: "密钥无效或已过期，请用 /config 更新当前服务商的密钥。",
    403: "当前密钥没有访问该接口或模型的权限。",
    404: "接口或模型不存在；请检查 Base URL 和模型名称。",
    429: "服务商限流或额度不足，请检查额度后再试。",
    500: "服务商内部错误，请稍后再试。",
    502: "服务商网关错误，请稍后再试。",
    503: "服务商暂时不可用，请稍后再试。",
    504: "服务商网关超时，请稍后再试。",
    529: "Anthropic 服务暂时过载，请稍后再试。",
}


class ChatClient:
    """One provider per instance. Never fallback to a different provider implicitly."""

    def __init__(self, config):
        config.validate()
        self.config = config
        self.opener = urllib.request.build_opener(NoRedirect)

    def request(self, endpoint, payload=None):
        body = json.dumps(payload, ensure_ascii=False).encode() if payload is not None else None
        headers = {"Accept": "application/json"}
        if self.config.protocol == "anthropic":
            headers.update({"x-api-key": self.config.api_key, "anthropic-version": self.config.anthropic_version})
        else:
            headers["Authorization"] = "Bearer " + self.config.api_key
        if body is not None:
            headers["Content-Type"] = "application/json"
        request = urllib.request.Request(self.config.base_url + endpoint, data=body, headers=headers)
        for attempt in range(3):
            try:
                with self.opener.open(request, timeout=60) as response:
                    raw = response.read(4_000_001)
                if len(raw) > 4_000_000:
                    raise RuntimeError("API 响应过大。")
                return json.loads(raw)
            except urllib.error.HTTPError as exc:
                status = exc.code
                retry_after = exc.headers.get("Retry-After", "") if exc.headers else ""
                exc.close()
                if status in {429, 500, 502, 503, 504, 529} and attempt < 2:
                    delay = min(float(retry_after), 5) if retry_after.isascii() and retry_after.isdigit() else 2 ** attempt
                    print(f"API 暂时返回 HTTP {status}，{delay:g} 秒后重试（{attempt + 1}/2）…", file=sys.stderr)
                    time.sleep(delay)
                    continue
                raise RuntimeError(f"API HTTP {status}：" + ERRORS.get(status, "请求失败，请检查当前服务商的接口配置。")) from None
            except (urllib.error.URLError, TimeoutError):
                raise RuntimeError("API 连接失败或请求超时。请检查网络和地址；未自动重试，避免重复提交。") from None
            except (json.JSONDecodeError, UnicodeDecodeError):
                raise RuntimeError("接口未返回有效 JSON，可能填写了网页地址。请检查 API Base URL。") from None

    def complete(self, messages, tools, tool_choice="auto"):
        if self.config.protocol == "anthropic":
            body = anthropic.payload(self.config, messages, tools, tool_choice)
            return anthropic.response(self.request("/messages", body))
        payload = {"model": self.config.model, "messages": messages}
        if tools:
            payload.update(tools=tools, tool_choice=tool_choice)
        data = self.request("/chat/completions", payload)
        try:
            choice = data["choices"][0]
            if choice.get("finish_reason") == "length":
                raise RuntimeError("模型输出被长度限制截断，未执行本轮工具；请缩小任务或更换模型。")
            message = choice["message"]
            if not isinstance(message, dict) or message.get("content") is not None and not isinstance(message["content"], str):
                raise ValueError
            if message.get("refusal"):
                raise RuntimeError("模型拒绝处理本次请求；未执行本轮工具。")
            return message
        except (KeyError, IndexError, TypeError, ValueError):
            raise RuntimeError("API 响应格式不兼容，预期 Chat Completions 消息。") from None

    def models(self):
        data = self.request("/models?limit=1000" if self.config.protocol == "anthropic" else "/models")
        try:
            entries = data["data"]
            if not isinstance(entries, list):
                raise ValueError
            return sorted({entry["id"] for entry in entries if isinstance(entry, dict) and isinstance(entry.get("id"), str)})[:500]
        except (KeyError, TypeError, ValueError):
            raise RuntimeError("服务商未返回兼容的模型列表；可用 /model 模型名称 手动设置。") from None

    def test(self):
        tool = {"type": "function", "function": {"name": "connection_check", "description": "Check API tool-call support without operating the computer.",
                "parameters": {"type": "object", "properties": {}, "required": [], "additionalProperties": False}}}
        choice = "auto" if self.config.protocol == "anthropic" else {"type": "function", "function": {"name": "connection_check"}}
        reply = self.complete([{"role": "user", "content": "Call connection_check with empty arguments."}], [tool], choice)
        for call in reply.get("tool_calls") or []:
            function = call.get("function") if isinstance(call, dict) else None
            if isinstance(function, dict) and function.get("name") == "connection_check":
                return "连接成功，模型已返回工具调用。测试未操作电脑。"
        return "API 已返回响应，但未确认工具调用能力；请更换支持工具调用的模型。"
