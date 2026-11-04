import sys
from dataclasses import replace

from . import __version__
from .agent import Agent
from .api import ChatClient
from .config import Config
from .records import record
from .setup_ui import configure
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


def status(config, tools, args, agent=None):
    print(f"\n  desk {__version__}  ·  {config.profile} / {config.model}\n  {tools.root}")
    print(f"  接口：{config.protocol}  API：{config.base_url}")
    print(f"  命令：{'开启' if args.allow_shell else '关闭'}  桌面：{'开启' if args.allow_desktop else '关闭'}  操作：{'自动批准' if args.yes else '执行前确认'}")
    print(f"  命令后端：{tools.shell_backend}")
    if agent is not None:
        print(f"  任务：{agent.state.value}")


def chat(agent, config, tools, args):
    try:
        import readline  # In-memory line editing/history; never writes a history file.
    except ImportError:
        pass
    status(config, tools, args, agent)
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
                status(config, tools, args, agent)
            elif task == "/new":
                agent = Agent(agent.client, tools, max_steps=args.max_steps)
                print("已开始新对话。")
            elif task == "/config":
                selected = configure(config.profile)
                if selected is None:
                    continue
                config = selected
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
                if selected is None:
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
                tools = Tools(args.workspace, tools.approve, args.allow_shell, args.allow_desktop, args.shell_backend)
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
