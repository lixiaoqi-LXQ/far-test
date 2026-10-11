#!/usr/bin/env python3
"""Optional matplotlib plot of measured record-size curves (not needed by RAF)."""
import json
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

rows = [json.loads(line) for line in Path("reports/raw/matrix-final.jsonl").read_text().splitlines()]
sizes = [64, 256, 4096, 65536, 1048576]
labels = ["64B", "256B", "4KiB", "64KiB", "1MiB"]
colors = {1: "#245d9d", 2: "#a47119", 4: "#248269", 8: "#9b4e2e", 16: "#73519d"}
fig, axes = plt.subplots(1, 2, figsize=(11, 4.2), layout="constrained")
for axis, mode, threads_list in ((axes[0], "read", (1, 2, 4, 8, 16)), (axes[1], "write", (1, 2, 4, 8))):
    for threads in threads_list:
        values = []
        for size in sizes:
            matching = [r for r in rows if r["mode"] == mode and r["record_size"] == str(size) and r["threads"] == threads
                        and (mode == "write" and r["batch"] == 256 and r["sync_ms"] == 1000
                             or mode == "read" and r["cache"] == "hot" and r["access"] == "sequential" and r["crc"])]
            if len(matching) != 1:
                raise ValueError("missing or duplicate plot input")
            values.append(matching[0]["bytes_per_second"] / 1e9)
        axis.plot(sizes, values, marker="o", lw=1.8, ms=4, color=colors[threads], label=f"{threads} thread{'s' if threads > 1 else ''}")
    axis.set_xscale("log", base=2)
    axis.set_xticks(sizes, labels)
    axis.set_ylim(bottom=0)
    axis.set_ylabel("Payload throughput (GB/s)")
    axis.set_xlabel("Record payload size")
    axis.grid(axis="y", alpha=0.25)
    axis.spines[["top", "right"]].set_visible(False)
    axis.legend(frameon=False, fontsize=8)
axes[0].set_title("Hot sequential read, CRC32C enabled")
axes[1].set_title("Durable batch write, 1s background sync")
fig.suptitle("RAF / ARM64 + NVMe: record-size scaling", fontsize=13)
fig.savefig("reports/performance-curves.svg")
fig.savefig("reports/performance-curves.png", dpi=160)
print("Generated reports/performance-curves.svg and .png")
