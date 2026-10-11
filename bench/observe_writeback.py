#!/usr/bin/env python3
"""Observe only this user's large RAF benchmark processes, without kernel changes."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, default=Path("build/raf_bench"))
    parser.add_argument("--output", type=Path, default=Path("reports/raw/writeback-observer.jsonl"))
    parser.add_argument("--max-seconds", type=float, default=60)
    args = parser.parse_args()
    exe = str(args.exe.resolve())
    start, last_seen, seen = time.monotonic(), time.monotonic(), set()
    with args.output.open("w") as output:
        while time.monotonic() - start < args.max_seconds:
            pids = subprocess.run(["pgrep", "-u", str(os.getuid()), "-x", "raf_bench"],
                                  capture_output=True, text=True).stdout.split()
            for pid in pids:
                try:
                    proc = Path("/proc") / pid
                    argv = (proc / "cmdline").read_bytes().decode().strip("\0").split("\0")
                    options = dict(zip(argv[1::2], argv[2::2]))
                    if argv[0] != exe or options.get("--mode") != "write" or int(options.get("--bytes", "0")) < 64 << 30:
                        continue
                    waits = Counter(task.joinpath("wchan").read_text().strip() for task in (proc / "task").iterdir())
                    thresholds = dict(line.split() for line in Path("/proc/vmstat").read_text().splitlines()
                                      if line.startswith(("nr_dirty_threshold ", "nr_dirty_background_threshold ")))
                    row = dict(second=time.monotonic() - start, pid=int(pid), sync_ms=int(options["--sync-ms"]),
                               thread_waits=dict(waits), thresholds=thresholds)
                    output.write(json.dumps(row) + "\n"); output.flush()
                    seen.add(pid); last_seen = time.monotonic()
                except (FileNotFoundError, ProcessLookupError):
                    pass
            if len(seen) >= 2 and time.monotonic() - last_seen > 1:
                break
            time.sleep(0.05)
    print(f"Observed {len(seen)} large-write processes")


if __name__ == "__main__":
    main()
