#!/usr/bin/env python3
"""Check that benchmark byte counters describe actual, fully consumed payloads."""
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def crc32c(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def fixture(path, payload):
    length = struct.pack("<I", len(payload))
    record = length + struct.pack("<I", crc32c(length)) + payload + struct.pack("<I", crc32c(payload))
    header = struct.pack("<IQ", 1, 16 + len(record))
    index = struct.pack("<Q", 16)
    with path.open("wb") as output:
        output.write(header + struct.pack("<I", crc32c(header)) + record + index + struct.pack("<I", crc32c(index)))
        output.flush()
        os.fsync(output.fileno())


def main():
    exe = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="raf-bench-test-") as scratch:
        path = Path(scratch) / "fixture"
        for size in (8, 9, 79):
            fixture(path, bytes(size - 1) + b"\x01")
            for crc in (0, 1):
                result = json.loads(subprocess.check_output(
                    [exe, "--mode", "read", "--path", str(path), "--crc", str(crc),
                     "--seconds", "0.02", "--threads", "1"], text=True))
                assert result["sink"] != 0, f"payload tail is never consumed (size={size}, crc={crc})"
                assert result["payload_bytes"] == size * result["records"]
                assert math.isclose(result["bytes_per_second"] * result["seconds"], result["payload_bytes"], rel_tol=1e-8)
        result = json.loads(subprocess.check_output(
            [exe, "--mode", "write", "--path", str(path) + "-write", "--size", "4096",
             "--bytes", "32768", "--seconds", "0", "--sync-ms", "0", "--threads", "1"], text=True))
        assert result["payload_bytes"] == 32768 and result["records"] == 8
        assert Path(str(path) + "-write.0").stat().st_size == 16 + 8 * (4096 + 12) + 8 * 8 + 4
        assert result["successful_syncs"] == 2
        assert result["durable_seconds"] >= result["append_seconds"] > 0
        assert result["sync_total_ns"] >= result["sync_max_ns"] > 0
    print("PASS benchmark consumes payload tails and reports actual byte/durability counters")


if __name__ == "__main__":
    main()
