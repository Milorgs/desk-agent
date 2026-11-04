"""Interchangeable command runners with the same output and cancellation rules."""
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import tempfile


def environment():
    return {key: value for key, value in os.environ.items()
            if not any(word in key.upper() for word in ("KEY", "TOKEN", "SECRET", "PASSWORD"))}


def native_path():
    return Path(__file__).resolve().parent / "_native/desk-runner"


def run_c(root, command, timeout, binary=None):
    runner = Path(binary) if binary else native_path()
    if not runner.is_file():
        raise RuntimeError("C 后端尚未构建，请运行 python3 install.py --with-native 或 python3 native/build.py。")
    process = subprocess.Popen([str(runner), str(root), str(max(1, int(timeout * 1000))), command],
                               env=environment(), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               stdin=subprocess.DEVNULL, start_new_session=True)
    try:
        output, error = process.communicate(timeout=timeout + 3)
    except BaseException as exc:
        process.terminate()
        try:
            process.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
        if isinstance(exc, subprocess.TimeoutExpired):
            raise RuntimeError("C 执行器未按时退出，已停止；未重新执行命令。") from None
        raise
    if process.returncode != 0:
        raise RuntimeError(f"C 执行器异常退出（{process.returncode}）；未重新执行命令。")
    header, separator, data = output.partition(b"\n")
    if not separator or len(data) > 16000:
        raise RuntimeError("C 执行器输出格式无效。")
    try:
        metadata = json.loads(header)
        if set(metadata) != {"exit_code", "timed_out", "truncated"}:
            raise ValueError
    except (ValueError, TypeError):
        raise RuntimeError("C 执行器返回无效元数据。") from None
    return dict(metadata, output=data.decode("utf-8", errors="replace"))


def run_python(root, command, timeout):
    with tempfile.TemporaryFile() as output:
        def limit_output():
            resource.setrlimit(resource.RLIMIT_FSIZE, (2_000_000, 2_000_000))

        process = subprocess.Popen(["/bin/bash", "--noprofile", "--norc", "-c", command],
                                   cwd=root, env=environment(), stdin=subprocess.DEVNULL,
                                   stdout=output, stderr=subprocess.STDOUT, start_new_session=True,
                                   preexec_fn=limit_output)
        timed_out = False
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        output.seek(0)
        data = output.read(16001)
    return {"exit_code": process.returncode, "timed_out": timed_out,
            "output": data[:16000].decode("utf-8", errors="replace"), "truncated": len(data) > 16000}
