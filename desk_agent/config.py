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
    # Write credentials first: a newly added profile must not become active without its key.
    private_write(auth_path(), json.dumps({"profiles": keys}) + "\n")
    private_write(config_path(), text)


def check_profile_name(name):
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,40}", name):
        raise ValueError("服务商配置名请用 1–40 位字母、数字、短横线或下划线。")


def normalize_url(value):
    value = value.strip().rstrip("/")
    if value.endswith("/chat/completions"):
        value = value[:-len("/chat/completions")]
    return value


@dataclass
class Config:
    base_url: str
    model: str
    api_key: str
    profile: str = "default"

    def __post_init__(self):
        self.base_url = normalize_url(self.base_url)

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
                   value("DESK_API_KEY", keys.get(name, "")), name)

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
        profiles[self.profile] = {"base_url": self.base_url, "model": self.model}
        keys[self.profile] = self.api_key
        write_store(self.profile, profiles, keys)

    def validate(self):
        check_profile_name(self.profile)
        url = urlsplit(self.base_url)
        if url.username or url.password or url.query or url.fragment:
            raise ValueError("API 地址不能包含账号、密码、查询参数或片段。")
        if url.scheme != "https" and not (
            url.scheme == "http" and url.hostname in {"localhost", "127.0.0.1", "::1"}
        ):
            raise ValueError("API 必须使用 HTTPS；仅本机服务允许 HTTP。")
        if not url.hostname:
            raise ValueError("API 地址缺少主机名。")
        if url.path.endswith("/responses") or url.path.endswith("/messages"):
            raise ValueError("当前支持 Chat Completions 协议，请填写兼容接口的 Base URL。")
        if not self.model or not self.model.strip():
            raise ValueError("尚未配置模型，请运行 desk 或 desk config。")
        if not self.api_key or not self.api_key.strip():
            raise ValueError("尚未配置 API Key，请运行 desk config。")
