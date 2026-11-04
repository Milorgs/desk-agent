#!/usr/bin/env python3
"""Install a standalone user-local desk command without pip or root."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import sys
import tempfile
import uuid


MARKER = "# desk-agent managed launcher"


def install(prefix, setup_path=False):
    if sys.version_info < (3, 11):
        raise RuntimeError("需要 Python 3.11 或更新版本。")
    source = Path(__file__).resolve().parent
    prefix = Path(prefix).expanduser().resolve()
    bindir = prefix / "bin"
    launcher = bindir / "desk"
    if launcher.is_symlink() or launcher.exists() and MARKER not in launcher.read_text(errors="replace"):
        raise RuntimeError(f"已有其他 desk 命令，未覆盖：{launcher}")
    app = prefix / "share/desk-agent/releases" / uuid.uuid4().hex
    app.mkdir(parents=True)
    shutil.copytree(source / "desk_agent", app / "desk_agent", ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    bindir.mkdir(parents=True, exist_ok=True)
    # A shell launcher avoids shebang path-length and space limitations.
    script = ("#!/bin/sh\n" + MARKER + "\nexec " + shlex.quote(sys.executable)
              + " -c " + shlex.quote("import sys; sys.path.insert(0, " + repr(str(app))
                                     + "); from desk_agent.cli import main; raise SystemExit(main())")
              + ' "$@"\n')
    fd, tmp = tempfile.mkstemp(dir=bindir, prefix=".desk-install-")
    try:
        with os.fdopen(fd, "w") as handle:
            handle.write(script)
        os.chmod(tmp, 0o755)
        os.replace(tmp, launcher)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)
    if setup_path and str(bindir) not in os.environ.get("PATH", "").split(":"):
        shell = Path(os.environ.get("SHELL", "/bin/bash")).name
        if shell in {"bash", "zsh"}:
            rc = Path.home() / (".bashrc" if shell == "bash" else ".zshrc")
            line = "export PATH=" + shlex.quote(str(bindir)) + ':"$PATH"'
            content = rc.read_text() if rc.exists() else ""
            if line not in content:
                with rc.open("a") as handle:
                    handle.write("\n# desk-agent command\n" + line + "\n")
            print(f"命令目录已加入 {rc}，新开终端生效。")
        else:
            print(f"请将 {bindir} 加入 {shell} 的 PATH。")
    print(f"安装完成：{launcher}\n输入 desk 开始；首次启动会引导配置 API。")
    return launcher


def main():
    p = argparse.ArgumentParser(description="安装 desk 命令到用户目录，无需 pip 或管理员权限")
    p.add_argument("--prefix", default=str(Path.home() / ".local"))
    p.add_argument("--setup-path", action="store_true", help="必要时将命令目录加入 Bash/Zsh 配置")
    args = p.parse_args()
    try:
        install(args.prefix, args.setup_path)
    except (OSError, RuntimeError) as exc:
        p.exit(1, str(exc) + "\n")


if __name__ == "__main__":
    main()
