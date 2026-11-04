"""Small, local process-launch benchmark; not an AI latency benchmark."""
import json
from pathlib import Path
import platform
import statistics
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from native.build import build
from desk_agent.process import run_c, run_python


def main():
    build()
    durations = {"python": [], "c": []}
    with tempfile.TemporaryDirectory() as root:
        for _ in range(30):
            for name, runner in [("python", run_python), ("c", run_c)]:
                start = time.perf_counter()
                result = runner(root, "true", 5)
                elapsed = (time.perf_counter() - start) * 1000
                if result["exit_code"] != 0:
                    raise RuntimeError("基准命令未成功。")
                durations[name].append(elapsed)
    print(json.dumps({"platform": platform.system() + " " + platform.machine(),
                      "command": "true", "samples_per_backend": 30,
                      "median_ms": {name: round(statistics.median(values), 3) for name, values in durations.items()},
                      "note": "Includes process startup; excludes compilation, API requests and desktop operations."}, indent=2))


if __name__ == "__main__":
    main()
