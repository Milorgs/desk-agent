import sys

from . import __version__
from .agent import Agent
from .api import ChatClient
from .config import Config
from .conversation import chat
from .setup_ui import choose_protocol, configure


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
                protocol = choose_protocol()
                if protocol is None:
                    continue
                profiles = Config.profiles()[1]
                number = 1
                while f"model-{number}" in profiles:
                    number += 1
                name = input(f"模型配置名称 [model-{number}]（输入 0 返回）：").strip()
                if name == "0":
                    continue
                selected = configure(name or f"model-{number}", new=True, protocol=protocol)
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
