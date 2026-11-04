import argparse
import getpass
import importlib.util
import json
import os
from pathlib import Path
import sys
import uuid

from . import __version__
from .agent import Agent
from .api import ChatClient
from .config import Config, auth_path, config_path
from .tools import Tools


HELP = '''直接输入需求，回车执行；可以接着追问。
/help                 显示帮助
/new                  清空当前对话
/status               查看模型、目录和工具状态
/config               修改 API、模型与密钥（重置对话）
/desktop on|off       开关桌面工具
/permissions ask|auto 操作前询问 / 自动批准已启用工具
/exit                 退出
Ctrl+C 停止当前任务，Ctrl+D 退出。'''


def parser():
    p = argparse.ArgumentParser(description="desk：输入一句话，操作你的 Linux 电脑")
    p.add_argument("task", nargs="?", help="留空进入持续对话；或输入任务、doctor、config、inspect")
    p.add_argument("--version", action="version", version=__version__)
    p.add_argument("--workspace", default=".", help="工作目录，默认当前目录")
    shell = p.add_mutually_exclusive_group()
    shell.add_argument("--allow-shell", dest="allow_shell", action="store_true", help="启用命令工具（默认启用，执行前确认）")
    shell.add_argument("--no-shell", dest="allow_shell", action="store_false", help="关闭命令工具")
    p.set_defaults(allow_shell=True)
    p.add_argument("--allow-desktop", action="store_true", help="启用桌面读取，目标应用文字会发送到模型 API")
    p.add_argument("--yes", action="store_true", help="自动批准已启用工具的写入和操作")
    p.add_argument("--max-steps", type=int, default=20, choices=range(1, 101), metavar="1..100")
    p.add_argument("--app", default="", help="inspect 的应用名称")
    p.add_argument("--record", action="store_true", help="保存本机私有对话记录")
    return p


def configure():
    if not sys.stdin.isatty():
        raise ValueError("首次配置需要交互终端，请运行 desk config；自动化场景可设置 DESK_BASE_URL、DESK_MODEL、DESK_API_KEY。")
    current = Config.load()
    print("\n配置模型 · 密钥输入不显示，保存在本机权限为 600 的文件中。")
    base = input(f"API 地址 [{current.base_url}]：").strip() or current.base_url
    model = input(f"模型名称 [{current.model or '必填'}]：").strip() or current.model
    key = getpass.getpass("API Key" + (" [回车保留]" if current.api_key else "") + "：").strip() or current.api_key
    config = Config(base.rstrip("/"), model, key)
    config.save()
    print(f"已保存。之后直接输入 desk 即可。\n配置：{config_path()}\n密钥：{auth_path()}")
    return config


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
    print("桌面目前支持 AT-SPI 控件；尚无截图识别、全局鼠标键盘及持久后台服务。")


def record(agent, config):
    directory = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")) / "desk-agent/sessions"
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    directory.chmod(0o700)
    destination = directory / (uuid.uuid4().hex + ".json")
    transcript = json.dumps({"version": __version__, "messages": agent.messages}, ensure_ascii=False, indent=2)
    if config.api_key:
        transcript = transcript.replace(config.api_key, "[REDACTED_API_KEY]")
    fd = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as handle:
        handle.write(transcript)
    print(f"记录：{destination}（分享前检查其中的文件/桌面内容）")


def status(config, tools, args):
    print(f"\n  desk {__version__}  ·  {config.model}\n  {tools.root}")
    print(f"  命令：{'开启' if args.allow_shell else '关闭'}  桌面：{'开启' if args.allow_desktop else '关闭'}  操作：{'自动批准' if args.yes else '执行前确认'}")


def chat(agent, config, tools, args):
    try:
        import readline  # In-memory line editing/history; never writes a history file.
    except ImportError:
        pass
    status(config, tools, args)
    print("\n输入需求开始，/help 查看帮助，/exit 退出。\n任务和工具结果会发送到你配置的 API。")
    while True:
        try:
            task = input("\n› ").strip()
            if not task:
                continue
            if task in {"/exit", "/quit"}:
                break
            if task == "/help":
                print(HELP)
            elif task == "/status":
                status(config, tools, args)
            elif task == "/new":
                agent = Agent(agent.client, tools, max_steps=args.max_steps)
                print("已开始新对话。")
            elif task == "/config":
                config = configure()
                agent = Agent(ChatClient(config), tools, max_steps=args.max_steps)
                print("新配置已启用，对话已重置。")
            elif task.startswith("/desktop "):
                value = task.split()[1:]
                if value not in [["on"], ["off"]]:
                    print("用法：/desktop on 或 /desktop off")
                    continue
                args.allow_desktop = value == ["on"]
                tools = Tools(args.workspace, tools.approve, args.allow_shell, args.allow_desktop)
                agent.tools = tools
                print("桌面工具已开启；读取的应用文字会发送至模型 API。" if args.allow_desktop else "桌面工具已关闭。")
            elif task.startswith("/permissions "):
                mode = task.split()[1:]
                if mode not in [["ask"], ["auto"]]:
                    print("用法：/permissions ask 或 /permissions auto")
                    continue
                args.yes = mode == ["auto"]
                print("已自动批准启用工具的操作。" if args.yes else "已恢复操作前确认。")
            elif task.startswith("/"):
                print("未知命令，输入 /help 查看帮助。")
            else:
                try:
                    agent.run(task)
                finally:
                    if args.record:
                        record(agent, config)
        except EOFError:
            break
        except KeyboardInterrupt:
            print("\n已停止当前操作，可以继续输入。已完成的修改不会自动撤销。")
        except (RuntimeError, ValueError, OSError) as exc:
            print("错误：" + str(exc), file=sys.stderr)
    print("再见。")


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        if args.task == "doctor":
            doctor()
            return 0
        if args.task == "config":
            configure()
            return 0

        def approve(message):
            print("\n" + message)
            if args.yes:
                return True
            if not sys.stdin.isatty():
                return False
            return input("允许？[y/N] ").strip().lower() == "y"

        tools = Tools(args.workspace, approve, args.allow_shell, args.allow_desktop)
        if args.task == "inspect":
            print(json.dumps(tools.desktop.snapshot(args.app), ensure_ascii=False, indent=2))
            return 0
        config = Config.load()
        try:
            config.validate()
        except ValueError:
            config = configure()
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
