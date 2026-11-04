import os
import json
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


@dataclass
class Config:
    base_url: str
    model: str
    api_key: str

    @classmethod
    def load(cls):
        path = config_path()
        data = tomllib.loads(path.read_text()) if path.exists() else {}
        model = data.get("model", {})
        auth = json.loads(auth_path().read_text()) if auth_path().exists() else {}
        return cls(
            os.environ.get("DESK_BASE_URL", model.get("base_url", "https://api.openai.com/v1")).rstrip("/"),
            os.environ.get("DESK_MODEL", model.get("name", "")),
            os.environ.get("DESK_API_KEY", auth.get("api_key", "")),
        )

    def save(self):
        self.validate()
        private_write(config_path(), "[model]\nbase_url = " + json.dumps(self.base_url) + "\nname = " + json.dumps(self.model) + "\n")
        private_write(auth_path(), json.dumps({"api_key": self.api_key}) + "\n")

    def validate(self):
        url = urlsplit(self.base_url)
        if url.username or url.password or url.query or url.fragment:
            raise ValueError("API 地址不能包含账号、密码、查询参数或片段。")
        if url.scheme != "https" and not (
            url.scheme == "http" and url.hostname in {"localhost", "127.0.0.1", "::1"}
        ):
            raise ValueError("API 必须使用 HTTPS；仅本机服务允许 HTTP。")
        if not url.hostname:
            raise ValueError("API 地址缺少主机名。")
        if not self.model:
            raise ValueError("尚未配置模型，请运行 desk 或 desk config。")
        if not self.api_key:
            raise ValueError("尚未配置 API Key，请运行 desk config。")
