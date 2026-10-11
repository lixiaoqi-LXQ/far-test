#!/usr/bin/env python3
"""RAF benchmark suites; retain JSONL results and remove only owned scratch files."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

SIZES = [64, 256, 4096, 65536, 1048576, "mixed"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, default=Path("build/raf_bench"))
    parser.add_argument("--suite", choices=["smoke", "baseline", "matrix", "sustained"], default="matrix")
    parser.add_argument("--output", type=Path, default=Path("reports/raw/raf.jsonl"))
    parser.add_argument("--write-bytes", type=int, default=256 << 20)
    parser.add_argument("--file-bytes", type=int, default=512 << 20)
    parser.add_argument("--read-seconds", type=float, default=0.4)
    parser.add_argument("--sustained-bytes", type=int, default=64 << 30)
    parser.add_argument("--sustained-threads", type=int, default=8)
    parser.add_argument("--device", default="nvme0n1")
    parser.add_argument("--repeat", type=int, default=1)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    exe = str(args.exe.resolve())

    with tempfile.TemporaryDirectory(prefix="raf-bench-", dir=".") as scratch:
        root = Path(scratch)

        def run(tag, **options):
            command = [exe, "--device", args.device]
            for key, value in options.items():
                command.extend(["--" + key.replace("_", "-"), str(value)])
            result = subprocess.run(command, check=True, capture_output=True, text=True)
            data = json.loads(result.stdout)
            if data["payload_bytes"] <= 0 or data["records"] <= 0 or data["seconds"] <= 0:
                raise RuntimeError("invalid benchmark counters")
            data.update(tag=tag, command=command)
            with args.output.open("a") as output:
                output.write(json.dumps(data) + "\n")
            print(f"{tag}: {data['bytes_per_second'] / 1e9:.3f} GB/s, "
                  f"{data['records_per_second']:.0f} records/s", flush=True)
            return data

        def write(tag, size, threads, batch, sync_ms, budget=None, seconds=0, keep=False):
            path = root / tag
            data = run(tag, mode="write", path=path, size=size, threads=threads,
                       batch=batch, sync_ms=sync_ms, bytes=budget or args.write_bytes, seconds=seconds)
            # Smoke tests verify the produced archive with the full-validation probe.
            if args.suite == "smoke":
                tests = args.exe.resolve().parent / "raf_tests"
                for thread in range(threads):
                    subprocess.run([str(tests), "--probe", "reader", str(path) + f".{thread}"], check=True)
            if not keep:
                for thread in range(threads):
                    Path(str(path) + f".{thread}").unlink()
            return Path(str(path) + ".0"), data

        if args.suite == "smoke":
            for size in (64, 4096, 1048576, "mixed"):
                path, _ = write(f"smoke-{size}", size, 2, 256, 10, 8 << 20, keep=True)
                for crc in (0, 1):
                    run(f"smoke-read-{size}-crc{crc}", mode="read", path=path, threads=2,
                        size=size, crc=crc, seconds=0.05, cache="hot", access="random")
        elif args.suite == "sustained":
            for sync in (0, 1000):
                write(f"sustained-1M-n{args.sustained_threads}-sync{sync}", 1048576,
                      args.sustained_threads, 256, sync,
                      args.sustained_bytes, seconds=120)
        elif args.suite == "baseline":
            for iteration in range(args.repeat):
                for size in (64, 4096, 1048576):
                    run(f"crc-{size}-r{iteration}", mode="crc", size=size, seconds=0.5)
                for size in (4096, 1048576):
                    for threads in (1, 8):
                        for batch in (1, 256):
                            write(f"write-{size}-n{threads}-b{batch}-r{iteration}", size, threads, batch, 1000)
                    path, _ = write(f"dataset-{size}-r{iteration}", size, 1, 256, 0, args.file_bytes, keep=True)
                    for threads in (1, 8):
                        for access in ("random", "sequential"):
                            run(f"read-{size}-{access}-n{threads}-r{iteration}", mode="read", path=path,
                                size=size, threads=threads, access=access, crc=1, cache="hot", seconds=args.read_seconds)
        else:
            for iteration in range(args.repeat):
                for size in SIZES:
                    for threads in (1, 2, 4, 8):
                        for batch in (1, 256):
                            for sync in (0, 1000):
                                write(f"write-{size}-n{threads}-b{batch}-sync{sync}-r{iteration}", size, threads, batch, sync)
                    path, _ = write(f"dataset-{size}-r{iteration}", size, 1, 256, 0, args.file_bytes, keep=True)
                    for threads in (1, 2, 4, 8, 16):
                        for access in ("random", "sequential"):
                            for crc in (0, 1):
                                run(f"read-{size}-{access}-n{threads}-crc{crc}-r{iteration}", mode="read", path=path,
                                    size=size, threads=threads, access=access, crc=crc, cache="hot", seconds=args.read_seconds)
                    for threads in (1, 8):
                        for access in ("random", "sequential"):
                            run(f"cold-{size}-{access}-n{threads}-r{iteration}", mode="read", path=path, size=size,
                                threads=threads, access=access, crc=1, cache="cold", seconds=0)


if __name__ == "__main__":
    main()
