"""Build the optional Linux process runner using the system C compiler."""
from pathlib import Path
import shutil
import subprocess


def build(destination=None):
    root = Path(__file__).resolve().parents[1]
    destination = Path(destination) if destination else root / "desk_agent/_native/desk-runner"
    compiler = shutil.which("cc")
    if not compiler:
        raise RuntimeError("未找到 C 编译器 cc，请先安装系统 C 开发工具。")
    destination.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    str(root / "native/runner.c"), "-o", str(destination)], check=True)
    return destination


if __name__ == "__main__":
    print(build())
