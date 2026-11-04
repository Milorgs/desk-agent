import os
import tomllib
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urlsplit


def config_path():
    return Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "desk-agent/config.toml"


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
        return cls(
            os.environ.get("DESK_BASE_URL", model.get("base_url", "https://api.openai.com/v1")).rstrip("/"),
            os.environ.get("DESK_MODEL", model.get("name", "")),
            os.environ.get("DESK_API_KEY", ""),
        )

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
            raise ValueError("请运行 config 或设置 DESK_MODEL。")
        if not self.api_key:
            raise ValueError("请在终端设置 DESK_API_KEY；不要把密钥写进仓库。")
