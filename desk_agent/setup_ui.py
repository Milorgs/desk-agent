import getpass
import sys
from dataclasses import replace
from urllib.parse import urlsplit

from .config import Config, auth_path, check_profile_name, config_path
from .api import ChatClient


def choose_protocol(current="openai"):
    print("\n  [1] OpenAI接口\n  [2] Anthropic接口\n  [0] 退出")
    default = "2" if current == "anthropic" else "1"
    while True:
        choice = input(f"\n请选择 [{default}]：").strip().lower() or default
        if choice == "0":
            return None
        protocol = {"1": "openai", "2": "anthropic", "openai": "openai", "anthropic": "anthropic"}.get(choice)
        if protocol:
            return protocol
        print("请输入 1、2 或 0。")


def configure(profile=None, *, new=False, protocol=None):
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
    if protocol is None:
        protocol = choose_protocol(current.protocol)
    if protocol is None:
        return None
    if protocol not in {"openai", "anthropic"}:
        raise ValueError("不支持的接口类型。")
    print("\n配置模型 · 密钥输入不显示，保存在本机权限为 600 的文件中。")
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
    config.validate()
    print("\n  [1] 测试连接并保存（少量 API 用量）\n  [2] 直接保存，稍后测试\n  [0] 取消")
    while True:
        action = input("请选择 [1]：").strip() or "1"
        if action == "0":
            return None
        if action == "1":
            result = ChatClient(config).check()
            print(result.message)
            if not result.success:
                raise RuntimeError("测试未通过，原配置未改动。")
            break
        if action == "2":
            print("将保存配置，尚未验证连接。")
            break
        print("请输入 1、2 或 0。")
    config.save()
    print(f"已保存。之后直接输入 desk 即可。\n配置：{config_path()}\n密钥：{auth_path()}")
    return config
