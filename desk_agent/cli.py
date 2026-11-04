import argparse
import importlib.util
import json
import os
import sys

from . import __version__
from .agent import Agent
from .api import ChatClient
from .config import Config
from .conversation import chat, status
from .menu_ui import menu
from .records import record
from .setup_ui import choose_protocol, configure
from .tools import Tools
from .process import native_path


def parser():
    p = argparse.ArgumentParser(description="desk：输入一句话，操作你的 Linux 电脑")
    p.add_argument("task", nargs="?", help="留空打开主菜单；或输入任务、doctor、config、inspect")
    p.add_argument("--version", action="version", version=__version__)
    p.add_argument("--workspace", default=".", help="工作目录，默认当前目录")
    p.add_argument("--provider", help="本次使用指定的已保存服务商配置，不使用 DESK_* 环境变量覆盖")
    shell = p.add_mutually_exclusive_group()
    shell.add_argument("--allow-shell", dest="allow_shell", action="store_true", help="启用命令工具（默认启用，执行前确认）")
    shell.add_argument("--no-shell", dest="allow_shell", action="store_false", help="关闭命令工具")
    p.set_defaults(allow_shell=True)
    p.add_argument("--allow-desktop", action="store_true", help="启用桌面读取，目标应用文字会发送到模型 API")
    p.add_argument("--yes", action="store_true", help="自动批准已启用工具的写入和操作")
    p.add_argument("--max-steps", type=int, default=20, choices=range(1, 101), metavar="1..100")
    p.add_argument("--app", default="", help="inspect 的应用名称")
    p.add_argument("--shell-backend", choices=["python", "c"], default="python", help="命令执行后端（C 需先构建）")
    p.add_argument("--record", action="store_true", help="保存本机私有对话记录")
    return p


def doctor():
    print(f"desk {__version__} · Python {sys.version.split()[0]}")
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
    print("AT-SPI 依赖：", "可加载（实际应用需用 inspect 验证）" if ready else "缺失，文件与命令功能仍可使用")
    try:
        Config.load().validate()
        print("API 配置：完整（未联网验证）")
    except ValueError as exc:
        print("API 配置：", exc)
    print("C 命令后端：", "已构建" if native_path().is_file() else "未构建（默认 Python 不受影响）")
    print("桌面目前支持 AT-SPI 控件；尚无截图识别、全局鼠标键盘及持久后台服务。")


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        if args.task == "doctor":
            doctor()
            return 0
        if args.task == "config":
            configure(args.provider)
            return 0

        def approve(message):
            print("\n" + message)
            if args.yes:
                return True
            if not sys.stdin.isatty():
                return False
            return input("允许？[y/N] ").strip().lower() == "y"

        tools = Tools(args.workspace, approve, args.allow_shell, args.allow_desktop, args.shell_backend)
        if args.task == "inspect":
            print(json.dumps(tools.desktop.snapshot(args.app), ensure_ascii=False, indent=2))
            return 0
        if args.task is None and sys.stdin.isatty():
            return menu(args, tools)
        config = Config.load(args.provider, use_env=args.provider is None)
        try:
            config.validate()
        except ValueError:
            config = configure(args.provider)
            if config is None:
                return 0
        agent = Agent(ChatClient(config), tools, max_steps=args.max_steps)
        if args.task:
            status(config, tools, args)
            print("任务和工具结果会发送至配置的 API。")
            try:
                agent.run(args.task)
            finally:
                if args.record:
                    record(agent, config)
        else:
            chat(agent, config, tools, args)
        return 0
    except KeyboardInterrupt:
        print("\n已停止。", file=sys.stderr)
        return 130
    except (RuntimeError, ValueError, OSError, EOFError) as exc:
        print("错误：" + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
