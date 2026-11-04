import json
import os
from pathlib import Path
import uuid

from . import __version__


def record(agent, config):
    directory = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")) / "desk-agent/sessions"
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    directory.chmod(0o700)
    destination = directory / (uuid.uuid4().hex + ".json")
    transcript = json.dumps({"version": __version__, "state": agent.state.value,
                             "last_error": agent.last_error, "messages": agent.messages}, ensure_ascii=False, indent=2)
    if config.api_key:
        transcript = transcript.replace(config.api_key, "[REDACTED_API_KEY]")
    fd = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as handle:
        handle.write(transcript)
    print(f"记录：{destination}（分享前检查其中的文件/桌面内容）")
