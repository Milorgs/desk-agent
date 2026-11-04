import hashlib
import json
import os
from pathlib import Path
import stat
import tempfile
import uuid

from .desktop import Desktop
from .config import config_path
from .process import run_c, run_python


def schema(name, description, properties):
    return {"type": "function", "function": {
        "name": name, "description": description,
        "parameters": {"type": "object", "properties": properties,
                       "required": list(properties), "additionalProperties": False},
    }}


STRING = {"type": "string"}


class Tools:
    shell_timeout = 30

    def __init__(self, root, approve, allow_shell=False, allow_desktop=False, shell_backend="python"):
        if shell_backend not in {"python", "c"}:
            raise ValueError("命令后端只能是 python 或 c。")
        self.shell_backend = shell_backend
        self.root = Path(root).resolve(strict=True)
        if not self.root.is_dir():
            raise ValueError("工作目录必须是文件夹。")
        self.approve = approve
        self.allow_shell = allow_shell
        self.allow_desktop = allow_desktop
        self.desktop = Desktop()
        self.schemas = [
            schema("list_files", "List up to 200 entries in a workspace directory.", {"path": STRING}),
            schema("read_file", "Read a UTF-8 file (up to 64 KB) and its SHA256.", {"path": STRING}),
            schema("write_file", "Create or replace a UTF-8 file. For existing files, supply SHA256 from read_file; for new files use null. Existing bytes are backed up.",
                   {"path": STRING, "content": STRING, "expected_sha256": {"type": ["string", "null"]}}),
        ]
        if allow_shell:
            self.schemas.append(schema("run_shell", "Run a Linux shell command in the workspace (30s timeout). This is not sandboxed. Prefer file tools for file changes.", {"command": STRING}))
        if allow_desktop:
            self.schemas.extend([
                schema("desktop_snapshot", "Read accessible apps; pass empty app to list apps, then exact app name to read its controls. Desktop text will be sent to model API.", {"app": STRING}),
                schema("desktop_action", "Invoke an action advertised in latest snapshot. Always re-snapshot afterwards.", {"ref": STRING, "action": STRING}),
                schema("desktop_set_text", "Replace the entire contents of an editable control from latest snapshot. Re-snapshot to verify.", {"ref": STRING, "text": STRING}),
            ])

    def path(self, value):
        path = (self.root / value).resolve()
        if not path.is_relative_to(self.root):
            raise ValueError("文件工具只能访问工作目录内的路径。")
        if path.is_relative_to(config_path().parent.resolve()):
            raise ValueError("文件工具不访问 desk-agent 的模型配置和密钥。")
        parts = path.relative_to(self.root).parts
        if any(p in {".git", ".desk-agent", ".ssh", ".gnupg"} or p == ".env" or p.startswith(".env.") and p != ".env.example" for p in parts):
            raise ValueError("文件工具不访问凭据、Git 内部目录或本地备份。")
        return path

    def call(self, name, arguments):
        spec = next((x["function"] for x in self.schemas if x["function"]["name"] == name), None)
        try:
            if spec is None:
                raise ValueError("未知或未启用的工具。")
            if not isinstance(arguments, dict) or set(arguments) != set(spec["parameters"]["properties"]):
                raise ValueError("工具参数缺失或包含未知字段。")
            for key, value in arguments.items():
                if not isinstance(value, str) and not (key == "expected_sha256" and value is None):
                    raise ValueError("工具参数类型不正确。")
            if name == "list_files":
                directory = self.path(arguments["path"])
                result = []
                with os.scandir(directory) as entries:
                    for entry in entries:
                        try:
                            self.path(str(Path(entry.path).relative_to(self.root)))
                        except ValueError:
                            continue
                        result.append({"name": entry.name, "directory": entry.is_dir(follow_symlinks=False), "symlink": entry.is_symlink()})
                        if len(result) == 200:
                            return {"entries": result, "possibly_truncated": True}
                return {"entries": result, "possibly_truncated": False}
            if name == "read_file":
                path = self.path(arguments["path"])
                if not path.is_file():
                    raise ValueError("路径不是普通文件。")
                with path.open("rb") as handle:
                    content = handle.read(65537)
                if len(content) > 65536:
                    raise ValueError("第一版只读取不超过 64 KB 的文件。")
                return {"content": content.decode("utf-8"), "sha256": hashlib.sha256(content).hexdigest()}
            if name == "write_file":
                return self.write(**arguments)
            if name == "run_shell":
                return self.shell(arguments["command"])
            if name == "desktop_snapshot":
                return self.desktop.snapshot(arguments["app"])
            if not self.approve(name + " " + json.dumps(arguments, ensure_ascii=False)):
                return {"error": "用户拒绝此操作，不要换用其他工具绕过。", "denied": True}
            if name == "desktop_action":
                return self.desktop.perform(arguments["ref"], action=arguments["action"])
            return self.desktop.perform(arguments["ref"], text=arguments["text"])
        except EOFError:
            raise
        except Exception as exc:
            return {"error": str(exc)[:1200], "type": type(exc).__name__}

    def write(self, path, content, expected_sha256):
        target = self.path(path)
        data = content.encode("utf-8")
        if len(data) > 65536:
            raise ValueError("第一版只写入不超过 64 KB 的文件。")
        existed = target.exists()
        if existed and not target.is_file():
            raise ValueError("目标不是普通文件。")
        if existed and target.stat().st_size > 65536:
            raise ValueError("现有文件过大，无法在第一版覆盖。")
        old = target.read_bytes() if existed else None
        digest = hashlib.sha256(old).hexdigest() if existed else None
        if digest != expected_sha256:
            raise ValueError("文件已变化，或覆盖前未读取，请重新读取文件。")
        if not self.approve(f"写入 {path}（{len(data)} 字节）\n{content}"):
            return {"denied": True, "error": "用户拒绝写入，不要绕过。"}
        # Recheck after waiting for approval.
        if target.exists() != existed or existed and target.read_bytes() != old:
            raise ValueError("确认期间文件已变化，未写入。")
        backup = None
        if existed:
            backup_dir = self.root / ".desk-agent" / "backups"
            if (self.root / ".desk-agent").is_symlink() or backup_dir.is_symlink():
                raise ValueError("备份目录不能是符号链接。")
            backup_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
            backup_dir.chmod(0o700)
            backup = backup_dir / (uuid.uuid4().hex + ".bak")
            fd = os.open(backup, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, "wb") as handle:
                handle.write(old)
        target.parent.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=target.parent, prefix=".desk-write-")
        try:
            with os.fdopen(fd, "wb") as handle:
                handle.write(data)
            os.chmod(tmp, stat.S_IMODE(target.stat().st_mode) if existed else 0o644)
            os.replace(tmp, target)
        finally:
            if os.path.exists(tmp):
                os.unlink(tmp)
        return {"written": path, "sha256": hashlib.sha256(data).hexdigest(),
                "backup": str(backup.relative_to(self.root)) if backup else None}

    def shell(self, command):
        if not self.approve("执行命令（具有当前用户权限，不是隔离环境）：\n" + command):
            return {"denied": True, "error": "用户拒绝执行，不要绕过。"}
        runner = run_c if self.shell_backend == "c" else run_python
        return runner(self.root, command, self.shell_timeout)
