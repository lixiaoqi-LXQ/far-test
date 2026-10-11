#!/usr/bin/env python3
"""Non-destructive fio baseline: operates only on newly created scratch files."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", type=int, default=5)
    parser.add_argument("--size", default="2G")
    parser.add_argument("--output", type=Path, default=Path("reports/raw/fio"))
    parser.add_argument("--large-sequential", action="store_true", help="Recheck only direct 1MiB sequential reads/writes")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    summary = []
    with tempfile.TemporaryDirectory(prefix="fio-", dir=".") as scratch:
        common = ["fio", "--ioengine=psync", "--iodepth=1", "--thread=1",
                  "--size=" + args.size, "--directory=" + scratch,
                  "--filename_format=data.$jobnum", "--group_reporting=1",
                  "--output-format=json", "--eta=never"]
        subprocess.run(common + ["--name=prepare", "--rw=write", "--bs=1M",
                                  "--numjobs=8", "--direct=1", "--end_fsync=1"],
                       check=True, stdout=subprocess.DEVNULL)
        for direct in ((1,) if args.large_sequential else (1, 0)):
            for bs in (("1M",) if args.large_sequential else ("4k", "1M")):
                for jobs in (1, 8):
                    for operation in (("read", "write") if args.large_sequential else ("randread", "read", "write")):
                        name = f"{'direct' if direct else 'buffered'}-{operation}-{bs}-n{jobs}"
                        output = args.output / (name + ".json")
                        command = common + ["--name=" + name, "--rw=" + operation,
                                            "--bs=" + bs, "--numjobs=" + str(jobs),
                                            "--direct=" + str(direct), "--time_based=1",
                                            "--runtime=" + str(args.runtime),
                                            "--ramp_time=1", "--invalidate=1",
                                            "--end_fsync=1", "--output=" + str(output)]
                        print(name, flush=True)
                        subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
                        data = json.loads(output.read_text())
                        job = data["jobs"][0]
                        direction = "write" if operation == "write" else "read"
                        result = job[direction]
                        summary.append(dict(name=name, direct=direct, size=bs, threads=jobs,
                                            operation=operation, bytes_per_second=result["bw_bytes"],
                                            iops=result["iops"], runtime_ms=result["runtime"],
                                            cpu_percent=job["usr_cpu"] + job["sys_cpu"],
                                            command=command))
        (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
