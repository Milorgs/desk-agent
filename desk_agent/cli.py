import argparse
import importlib.util
import json
import os
from pathlib import Path
import sys
import uuid

from . import __version__
from .agent import Agent
from .api import ChatClient
from .config import Config, config_path
from .tools import Tools


def parser():
    p = argparse.ArgumentParser(description="desk-agent：Linux AI 文件、终端和桌面助手")
    p.add_argument("task", nargs="?", help="自然语言任务；doctor / config / inspect 为内置命令")
    p.add_argument("--version", action="version", version=__version__)
    p.add_argument("--workspace", default=".", help="文件工作目录，默认当前目录")
    p.add_argument("--allow-shell", action="store_true", help="向模型提供命令执行工具（不构成系统隔离）")
    p.add_argument("--allow-desktop", action="store_true", help="允许读取桌面文字并发送给配置的模型服务")
    p.add_argument("--yes", action="store_true", help="自动批准已启用工具的写入和操作，谨慎使用")
    p.add_argument("--max-steps", type=int, default=20, choices=range(1, 101), metavar="1..100")
    p.add_argument("--app", default="", help="inspect 的应用名称，不填时只列出应用")
    p.add_argument("--record", action="store_true", help="在本机私有状态目录保存完整对话及工具结果，用于复盘")
    return p


def configure():
    path = config_path()
    if path.exists():
        raise ValueError(f"配置已存在，请直接编辑：{path}")
    base = input("API Base URL（默认 https://api.openai.com/v1）：").strip() or "https://api.openai.com/v1"
    model = input("模型名称：").strip()
    Config(base, model, "validation-only").validate()
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as handle:
        handle.write("[model]\nbase_url = " + json.dumps(base) + "\nname = " + json.dumps(model) + "\n")
    print(f"配置已保存：{path}\n密钥请通过 DESK_API_KEY 环境变量设置。")


def doctor():
    print(f"desk-agent {__version__} · Python {sys.version.split()[0]}")
    print("桌面：", os.environ.get("XDG_CURRENT_DESKTOP", "未知"))
    print("会话：", os.environ.get("XDG_SESSION_TYPE", "未知"))
    ready = False
    if importlib.util.find_spec("gi"):
        try:
            import gi
            gi.require_version("Atspi", "2.0")
            ready = True
        except (ImportError, ValueError):
            pass
    print("AT-SPI 依赖：", "可加载（实际应用可访问性需用 inspect 验证）" if ready else "缺失")
    config = Config.load()
    try:
        config.validate()
        print("API 配置：完整（未联网验证）")
    except ValueError as exc:
        print("API 配置：", exc)
    print("v0.1：支持文件、限时命令、AT-SPI 控件；尚无截图识别、全局鼠标键盘及持久后台服务。")


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        if args.task == "doctor":
            doctor()
            return 0
        if args.task == "config":
            configure()
            return 0
        if not args.task:
            parser().print_help()
            return 0

        def approve(message):
            print("\n" + message)
            if args.yes:
                return True
            if not sys.stdin.isatty():
                return False
            return input("允许这次操作？[y/N] ").strip().lower() == "y"

        tools = Tools(args.workspace, approve, args.allow_shell, args.allow_desktop)
        if args.task == "inspect":
            print(json.dumps(tools.desktop.snapshot(args.app), ensure_ascii=False, indent=2))
            return 0
        config = Config.load()
        client = ChatClient(config)
        print(f"工作目录：{tools.root}\n模型：{config.model} · API：{config.base_url}")
        print("任务、读取的文件内容和工具结果会发送至此 API；默认不保存对话。")
        if args.allow_desktop:
            print("已启用桌面读取：目标应用中的文字也会发送至此 API。")
        agent = Agent(client, tools, max_steps=args.max_steps)
        try:
            agent.run(args.task)
        finally:
            if args.record:
                directory = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")) / "desk-agent/sessions"
                directory.mkdir(parents=True, exist_ok=True, mode=0o700)
                directory.chmod(0o700)
                destination = directory / (uuid.uuid4().hex + ".json")
                transcript = json.dumps({"version": __version__, "messages": agent.messages}, ensure_ascii=False, indent=2)
                transcript = transcript.replace(config.api_key, "[REDACTED_API_KEY]")
                fd = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                with os.fdopen(fd, "w") as handle:
                    handle.write(transcript)
                print(f"本次记录：{destination}（含任务与文件/桌面内容，分享前请检查）")
        return 0
    except KeyboardInterrupt:
        print("\n已停止。已完成的文件修改不会自动撤销。", file=sys.stderr)
        return 130
    except (RuntimeError, ValueError, OSError, EOFError) as exc:
        print("错误：" + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
