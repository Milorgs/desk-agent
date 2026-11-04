import os
import json
import re
import tempfile
import tomllib
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urlsplit


def config_path():
    return Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config") / "desk-agent/config.toml"


def auth_path():
    return config_path().with_name("auth.json")


def private_write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd, tmp = tempfile.mkstemp(prefix=".desk-config-", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as handle:
            handle.write(text)
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def read_store():
    path = config_path()
    data = tomllib.loads(path.read_text()) if path.exists() else {}
    auth = json.loads(auth_path().read_text()) if auth_path().exists() else {}
    if not isinstance(auth, dict):
        raise ValueError("密钥配置格式错误，请检查 auth.json。")
    profiles = dict(data.get("profiles", {}))
    keys = dict(auth.get("profiles", {}))
    # v0.1/v0.2 are read without rewriting anything until a successful save.
    if "model" in data and "default" not in profiles:
        profiles["default"] = {"base_url": data["model"].get("base_url", "https://api.openai.com/v1"),
                               "model": data["model"].get("name", "")}
    if auth.get("api_key") and "default" not in keys:
        keys["default"] = auth["api_key"]
    return data.get("active_profile", "default"), profiles, keys


def write_store(active, profiles, keys):
    text = "active_profile = " + json.dumps(active) + "\n"
    for name, profile in profiles.items():
        text += "\n[profiles." + json.dumps(name) + "]\n"
        text += "base_url = " + json.dumps(profile["base_url"]) + "\n"
        text += "model = " + json.dumps(profile["model"]) + "\n"
        text += "protocol = " + json.dumps(profile.get("protocol", "openai")) + "\n"
        if profile.get("protocol") == "anthropic":
            text += "max_tokens = " + str(profile.get("max_tokens", 4096)) + "\n"
            text += "anthropic_version = " + json.dumps(profile.get("anthropic_version", "2023-06-01")) + "\n"
    # Write credentials first: a newly added profile must not become active without its key.
    private_write(auth_path(), json.dumps({"profiles": keys}) + "\n")
    private_write(config_path(), text)


def check_profile_name(name):
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,40}", name):
        raise ValueError("服务商配置名请用 1–40 位字母、数字、短横线或下划线。")


def normalize_url(value, protocol="openai"):
    value = value.strip().rstrip("/")
    suffix = "/messages" if protocol == "anthropic" else "/chat/completions"
    if value.endswith(suffix):
        value = value[:-len(suffix)]
    if protocol == "anthropic" and not urlsplit(value).path:
        value += "/v1"
    return value


@dataclass
class Config:
    base_url: str
    model: str
    api_key: str
    profile: str = "default"
    protocol: str = "openai"
    max_tokens: int = 4096
    anthropic_version: str = "2023-06-01"

    def __post_init__(self):
        self.base_url = normalize_url(self.base_url, self.protocol)

    @classmethod
    def load(cls, profile=None, *, use_env=True):
        active, profiles, keys = read_store()
        name = profile or active
        if profile is not None and profile not in profiles:
            raise ValueError(f"没有服务商配置 {profile}；使用 /provider add {profile} 添加。")
        entry = profiles.get(name, {})
        def value(env, fallback):
            return (os.environ.get(env) or fallback) if use_env else fallback
        return cls(value("DESK_BASE_URL", entry.get("base_url", "https://api.openai.com/v1")),
                   value("DESK_MODEL", entry.get("model", "")),
                   value("DESK_API_KEY", keys.get(name, "")), name,
                   value("DESK_API_TYPE", entry.get("protocol", "openai")),
                   entry.get("max_tokens", 4096), entry.get("anthropic_version", "2023-06-01"))

    @classmethod
    def profiles(cls):
        active, profiles, _ = read_store()
        return active, profiles

    @classmethod
    def activate(cls, name):
        config = cls.load(name, use_env=False)
        config.validate()
        _, profiles, keys = read_store()
        write_store(name, profiles, keys)
        return config

    def save(self):
        self.validate()
        check_profile_name(self.profile)
        _, profiles, keys = read_store()
        profiles[self.profile] = {"base_url": self.base_url, "model": self.model,
                                  "protocol": self.protocol, "max_tokens": self.max_tokens,
                                  "anthropic_version": self.anthropic_version}
        keys[self.profile] = self.api_key
        write_store(self.profile, profiles, keys)

    def validate(self):
        check_profile_name(self.profile)
        if self.protocol not in {"openai", "anthropic"}:
            raise ValueError("接口类型只能是 openai 或 anthropic。")
        if type(self.max_tokens) is not int or not 1 <= self.max_tokens <= 65536:
            raise ValueError("max_tokens 必须是 1–65536 的整数。")
        if not isinstance(self.anthropic_version, str) or not re.fullmatch(r"\d{4}-\d{2}-\d{2}", self.anthropic_version):
            raise ValueError("anthropic_version 必须是 YYYY-MM-DD 格式。")
        url = urlsplit(self.base_url)
        if url.username or url.password or url.query or url.fragment:
            raise ValueError("API 地址不能包含账号、密码、查询参数或片段。")
        if url.scheme != "https" and not (
            url.scheme == "http" and url.hostname in {"localhost", "127.0.0.1", "::1"}
        ):
            raise ValueError("API 必须使用 HTTPS；仅本机服务允许 HTTP。")
        if not url.hostname:
            raise ValueError("API 地址缺少主机名。")
        if url.path.endswith(("/responses", "/messages", "/chat/completions")):
            raise ValueError("地址与接口类型不匹配，请检查 /config 中的接口类型和 API 地址。")
        if not self.model or not self.model.strip():
            raise ValueError("尚未配置模型，请运行 desk 或 desk config。")
        if not self.api_key or not self.api_key.strip():
            raise ValueError("尚未配置 API Key，请运行 desk config。")
