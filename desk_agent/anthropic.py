"""Native Messages API adapter, independent of local tools and HTTP transport."""
import json


def payload(config, messages, tools, tool_choice):
    system = []
    history = []

    def append(role, blocks):
        if not blocks:
            return
        if history and history[-1]["role"] == role:
            history[-1]["content"].extend(blocks)
        else:
            history.append({"role": role, "content": blocks})

    for message in messages:
        role = message["role"]
        if role == "system":
            if message.get("content"):
                system.append(message["content"])
        elif role == "user":
            append("user", [{"type": "text", "text": message["content"]}])
        elif role == "assistant":
            if "_anthropic_content" in message:
                # Keep signed thinking/redacted blocks unchanged for tool continuations.
                append("assistant", list(message["_anthropic_content"]))
            else:
                blocks = []
                if message.get("content"):
                    blocks.append({"type": "text", "text": message["content"]})
                for call in message.get("tool_calls", []):
                    function = call["function"]
                    blocks.append({"type": "tool_use", "id": call["id"], "name": function["name"],
                                   "input": json.loads(function["arguments"])})
                append("assistant", blocks)
        elif role == "tool":
            content = message["content"]
            try:
                result = json.loads(content)
                failed = isinstance(result, dict) and bool(result.get("error") or result.get("denied"))
            except (json.JSONDecodeError, TypeError):
                failed = False
            append("user", [{"type": "tool_result", "tool_use_id": message["tool_call_id"],
                             "content": content, "is_error": failed}])
        else:
            raise ValueError("Anthropic 消息包含不支持的角色。")
    body = {"model": config.model, "max_tokens": config.max_tokens, "messages": history}
    if system:
        body["system"] = "\n\n".join(system)
    if tools:
        body["tools"] = [{"name": t["function"]["name"],
                          "description": t["function"]["description"],
                          "input_schema": t["function"]["parameters"]} for t in tools]
        if isinstance(tool_choice, dict):
            body["tool_choice"] = {"type": "tool", "name": tool_choice["function"]["name"]}
        else:
            choices = {"auto": "auto", "required": "any", "none": "none"}
            if tool_choice not in choices:
                raise ValueError("不支持的 Anthropic 工具选择方式。")
            body["tool_choice"] = {"type": choices[tool_choice]}
    return body


def response(data):
    if not isinstance(data, dict):
        raise RuntimeError("Anthropic 响应必须是 JSON 对象。")
    stop = data.get("stop_reason")
    if stop in {"max_tokens", "model_context_window_exceeded"}:
        raise RuntimeError("Anthropic 响应被长度限制截断，本轮工具未执行；请缩小任务或调整 max_tokens。")
    if stop == "refusal":
        raise RuntimeError("模型拒绝处理本次请求；未执行本轮工具。")
    if stop not in {"end_turn", "tool_use", "stop_sequence"}:
        raise RuntimeError("Anthropic 返回了未完成或不支持的停止状态，本轮工具未执行。")
    blocks = data.get("content")
    if not isinstance(blocks, list):
        raise RuntimeError("Anthropic 响应缺少 content 数组。")
    texts, calls, ids = [], [], set()
    for block in blocks:
        if not isinstance(block, dict):
            raise RuntimeError("Anthropic 返回无效内容块。")
        kind = block.get("type")
        if kind == "text" and isinstance(block.get("text"), str):
            texts.append(block["text"])
        elif kind == "tool_use":
            call_id, name, arguments = block.get("id"), block.get("name"), block.get("input")
            if not isinstance(call_id, str) or not call_id or call_id in ids or not isinstance(name, str) or not name or not isinstance(arguments, dict):
                raise RuntimeError("Anthropic 工具调用格式无效，本轮工具未执行。")
            ids.add(call_id)
            calls.append({"id": call_id, "type": "function", "function": {
                "name": name, "arguments": json.dumps(arguments, ensure_ascii=False)}})
        elif kind not in {"thinking", "redacted_thinking"}:
            raise RuntimeError("Anthropic 返回不支持的内容块，本轮工具未执行。")
    if (stop == "tool_use") != bool(calls):
        raise RuntimeError("Anthropic 工具调用与停止状态不一致，本轮工具未执行。")
    result = {"role": "assistant", "content": "\n".join(texts) or None, "_anthropic_content": blocks}
    if calls:
        result["tool_calls"] = calls
    return result
