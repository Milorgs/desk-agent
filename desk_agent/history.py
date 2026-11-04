"""Bound API context using complete user turns; retain full local records."""
import json


def select_context(messages, tools, max_chars=128_000, keep_turns=6):
    starts = [i for i, message in enumerate(messages) if message["role"] == "user"]
    if not starts:
        return list(messages), 0
    prefix = messages[:starts[0]]
    current = messages[starts[-1]:]

    def size(history):
        return len(json.dumps({"messages": history, "tools": tools}, ensure_ascii=False))

    selected = prefix + current
    if size(selected) > max_chars:
        raise RuntimeError("本轮内容已超过上下文预算，请拆分任务、减少读取内容或用 /new 开始。当前操作不会继续。")
    first = len(starts) - 1
    for index in range(len(starts) - 2, max(-1, len(starts) - keep_turns - 2), -1):
        candidate = prefix + messages[starts[index]:]
        if size(candidate) > max_chars:
            break
        selected, first = candidate, index
    return selected, first
