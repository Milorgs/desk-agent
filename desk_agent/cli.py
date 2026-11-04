import argparse
import getpass
import importlib.util
import json
import os
from pathlib import Path
import sys
import uuid
from dataclasses import replace
from urllib.parse import urlsplit

from . import __version__
from .agent import Agent
from .api import ChatClient
from .config import Config, auth_path, check_profile_name, config_path
from .tools import Tools


HELP = '''直接输入需求，回车执行；可以接着追问。
/help                 显示帮助
/menu                 返回主菜单（保留当前对话）
/new                  清空当前对话
/status               查看模型、目录和工具状态
/config               修改 API、模型与密钥（重置对话）
/provider             列出已保存的服务商配置
/provider add 名称    添加独立的服务商配置并切换
/provider 名称        切换服务商并重置对话
/model                获取当前服务商的模型列表
/model 模型名称       切换并保存当前服务商的默认模型
/test                 测试 API 连接和工具调用（少量 API 用量）
/desktop on|off       开关桌面工具
/permissions ask|auto 操作前询问 / 自动批准已启用工具
/exit                 退出
Ctrl+C 停止当前任务，Ctrl+D 退出。'''


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
    p.add_argument("--record", action="store_true", help="保存本机私有对话记录")
    return p


def configure(profile=None, *, new=False):
    if not sys.stdin.isatty():
        raise ValueError("首次配置需要交互终端，请运行 desk config；自动化场景可设置 DESK_BASE_URL、DESK_MODEL、DESK_API_KEY。")
    if new:
        check_profile_name(profile)
        if profile in Config.profiles()[1]:
            raise ValueError("该服务商配置已存在；先切换，再用 /config 编辑。")
        current = Config("https://api.openai.com/v1", "", "", profile)
    else:
        if profile is not None and profile not in Config.profiles()[1]:
            current = Config.load()
            current.profile = profile
        else:
            current = Config.load(profile, use_env=profile is None)
    print("\n配置模型 · 密钥输入不显示，保存在本机权限为 600 的文件中。")
    print("  [1] OpenAI 兼容接口\n  [2] Anthropic 原生接口")
    protocol = input(f"接口类型 [{current.protocol}]（openai / anthropic）：").strip().lower() or current.protocol
    protocol = {"1": "openai", "2": "anthropic"}.get(protocol, protocol)
    if protocol not in {"openai", "anthropic"}:
        raise ValueError("接口类型请输入 openai 或 anthropic。")
    default_base = current.base_url if protocol == current.protocol else (
        "https://api.anthropic.com/v1" if protocol == "anthropic" else "https://api.openai.com/v1")
    base = input(f"API 地址 [{default_base}]：").strip() or default_base
    model = input(f"模型名称 [{current.model or '必填'}]：").strip() or current.model
    old_url, new_url = urlsplit(current.base_url), urlsplit(base)
    same_host = (old_url.scheme, old_url.hostname, old_url.port) == (new_url.scheme, new_url.hostname, new_url.port)
    previous_key = current.api_key if same_host else ""
    if current.api_key and not same_host:
        print("API 主机已改变，请输入新服务商的密钥。")
    key = getpass.getpass("API Key" + (" [回车保留]" if previous_key else "") + "：").strip() or previous_key
    config = replace(current, base_url=base, model=model, api_key=key, protocol=protocol)
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
    print(f"\n  desk {__version__}  ·  {config.profile} / {config.model}\n  {tools.root}")
    print(f"  接口：{config.protocol}  API：{config.base_url}")
    print(f"  命令：{'开启' if args.allow_shell else '关闭'}  桌面：{'开启' if args.allow_desktop else '关闭'}  操作：{'自动批准' if args.yes else '执行前确认'}")


def chat(agent, config, tools, args):
    try:
        import readline  # In-memory line editing/history; never writes a history file.
    except ImportError:
        pass
    status(config, tools, args)
    print("\n输入需求开始，/menu 返回主菜单，/help 查看帮助，/exit 退出。\n任务和工具结果会发送到你配置的 API。")
    while True:
        try:
            task = input("\n› ").strip()
            if not task:
                continue
            if task in {"/exit", "/quit"}:
                break
            if task == "/menu":
                return agent, config, tools
            if task == "/help":
                print(HELP)
            elif task == "/status":
                status(config, tools, args)
            elif task == "/new":
                agent = Agent(agent.client, tools, max_steps=args.max_steps)
                print("已开始新对话。")
            elif task == "/config":
                config = configure(config.profile)
                agent = Agent(ChatClient(config), tools, max_steps=args.max_steps)
                print("新配置已启用，对话已重置。")
            elif task == "/provider":
                _, profiles = Config.profiles()
                if not profiles:
                    print("尚无已保存服务商；使用 /provider add 名称 添加。")
                for name, entry in profiles.items():
                    print(f"{'*' if name == config.profile else ' '} {name}  [{entry.get('protocol', 'openai')}]  {entry['model']}  {entry['base_url']}")
            elif task.startswith("/provider "):
                parts = task.split()
                if len(parts) == 3 and parts[1] == "add":
                    selected = configure(parts[2], new=True)
                elif len(parts) == 2 and parts[1] != "add":
                    selected = Config.activate(parts[1])
                else:
                    print("用法：/provider 名称 或 /provider add 名称")
                    continue
                config = selected
                tools.desktop.refs.clear()
                agent = Agent(ChatClient(config), tools, max_steps=args.max_steps)
                print(f"已切换至 {config.profile} / {config.model}，历史对话已清空。")
            elif task in {"/model", "/models"}:
                print("正在获取当前服务商的模型列表…")
                models = agent.client.models()
                for name in models:
                    print(("* " if name == config.model else "  ") + name)
                print("用 /model 模型名称 切换；列表存在不代表该模型支持工具调用，可用 /test 验证。")
            elif task.startswith("/model "):
                model = task[len("/model "):].strip()
                if not model:
                    print("请输入完整模型名称。")
                    continue
                selected = replace(config, model=model)
                selected.save()
                config = selected
                tools.desktop.refs.clear()
                agent = Agent(ChatClient(config), tools, max_steps=args.max_steps)
                print(f"已切换模型：{model}，已保存并清空当前对话。可用 /test 检查是否可用。")
            elif task == "/test":
                print("发送最小工具调用测试，不发送文件、桌面或历史对话（会产生少量 API 用量）…")
                print(agent.client.test())
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


def select_model():
    active, profiles = Config.profiles()
    if not profiles:
        print("尚未保存模型，请在主菜单选择 [3] 添加模型。")
        return None
    names = list(profiles)
    print("\n选择已保存的模型：")
    for index, name in enumerate(names, 1):
        entry = profiles[name]
        current = " · 当前" if name == active else ""
        print(f"  [{index}] {name} / {entry['model']} ({entry.get('protocol', 'openai')}){current}")
    print("  [0] 返回")
    while True:
        choice = input("选择编号：").strip()
        if choice in {"", "0"}:
            return None
        if choice.isascii() and choice.isdigit() and 1 <= int(choice) <= len(names):
            return Config.activate(names[int(choice) - 1])
        print("请输入列表中的编号，或输入 0 返回。")


def menu(args, tools):
    config, agent = None, None
    try:
        loaded = Config.load(args.provider, use_env=args.provider is None)
        loaded.validate()
        config = loaded
    except (ValueError, OSError) as exc:
        if args.provider:
            print(f"未能选用指定模型：{exc}")
    while True:
        try:
            print(f"\n  desk {__version__}")
            print(f"  当前模型：{config.profile} / {config.model}" if config else "  当前模型：未配置")
            print("\n  [1] 开始对话\n  [2] 选择模型\n  [3] 添加模型\n  [0] 退出")
            choice = input("\n请选择 [1]：").strip() or "1"
            if choice in {"0", "/exit", "/quit"}:
                print("再见。")
                return 0
            if choice == "1":
                if config is None:
                    print("请先选择 [3] 添加模型，或选择 [2] 选用已保存的模型。")
                    continue
                if agent is None:
                    agent = Agent(ChatClient(config), tools, max_steps=args.max_steps)
                state = chat(agent, config, tools, args)
                if state is None:
                    return 0
                agent, config, tools = state
            elif choice == "2":
                selected = select_model()
                if selected:
                    config, agent = selected, None
                    tools.desktop.refs.clear()
                    print(f"已选用 {config.profile} / {config.model}，下一次对话将使用该模型。")
            elif choice == "3":
                profiles = Config.profiles()[1]
                number = 1
                while f"model-{number}" in profiles:
                    number += 1
                name = input(f"模型配置名称 [model-{number}]（输入 0 返回）：").strip()
                if name == "0":
                    continue
                selected = configure(name or f"model-{number}", new=True)
                config, agent = selected, None
                tools.desktop.refs.clear()
                print(f"已添加并选用 {config.profile} / {config.model}。")
            else:
                print("请输入 1、2、3 或 0。")
        except EOFError:
            print("\n再见。")
            return 0
        except KeyboardInterrupt:
            print("\n已取消，返回主菜单。")
        except (RuntimeError, ValueError, OSError) as exc:
            print("错误：" + str(exc), file=sys.stderr)


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

        tools = Tools(args.workspace, approve, args.allow_shell, args.allow_desktop)
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
