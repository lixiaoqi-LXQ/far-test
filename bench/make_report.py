#!/usr/bin/env python3
"""Build the performance report from retained raw measurements, without dependencies."""
import json
from pathlib import Path
from statistics import median

RAW = Path("reports/raw")


def read(name):
    return [json.loads(line) for line in (RAW / (name + ".jsonl")).read_text().splitlines()]


def pick(rows, **conditions):
    selected = [row for row in rows if all(row.get(key) == value for key, value in conditions.items())]
    if not selected:
        raise ValueError(f"missing measurements: {conditions}")
    return selected


def value(rows, field="bytes_per_second"):
    return median(row[field] for row in rows)


def cell(rows):
    return f"{value(rows, 'records_per_second'):,.0f} / {value(rows) / 1e6:,.1f}"


def main():
    matrix, focused = read("matrix-final"), read("focused")
    portable, hardware = read("portable-final"), read("hardware-final")
    sustained = read("sustained") + read("sustained-single")
    fio = json.loads((RAW / "fio/summary.json").read_text())
    fio_late = json.loads((RAW / "fio-final/summary.json").read_text())
    lines = [
        "# RAF 性能报告", "",
        "机器：Linux 6.17 / ARM64，10 个 Cortex-X925 + 10 个 Cortex-A725，121GiB RAM，Samsung MZALC4T0HBL1 4TB NVMe，ext4。GCC 13.3 / Release，CRC32C 实际路径为 ARM 硬件指令。CPU governor 为 performance，未固定线程亲和性；大小核调度和文件分配会产生波动。机器信息见 [environment.json](raw/environment.json)。", "",
        "RAF 始终使用 Buffered I/O。fio 3.36 同时测 Direct/Buffered 两组、4KiB/1MiB、1/8 jobs、顺序读/随机读/顺序写，每项运行 1s 后统计 5s，psync、iodepth=1、独立文件。原始 fio 配置使用 invalidate=1；Buffered 读在循环回到文件开头时也会驱逐缓存，因此这组结果属于循环驱逐缓存的存储路径基线。这个 1s 运行阶段不能保证热缓存。所有基准仅操作临时测试文件。参数语义参考 [fio 官方文档](https://fio.readthedocs.io/en/latest/fio_doc.html)。fio 覆写已准备文件，RAF 创建新文件并追加，两者还存在文件分配、校验和格式开销差异。", "",
        "下面带宽只统计 payload，MB/GB 均为十进制单位。读 TPS 是 Record 次数/秒；写 TPS 包含最终 seal 和两次同步。并发读共享一个 Reader 和同一个文件，并发写使用不同文件、每文件一个 Writer。热读前完整遍历预热，mincore 检查驻留；最终基准消费每个 payload 字节（64-bit XOR + 字节尾部），CRC 开/关采用相同消费方式。热读总吞吐属于逻辑 payload 处理速率，多个读者可共享缓存行，不能直接当作 DRAM 总线带宽。", "",
        "## 4KiB / 1MiB 主结果", "",
        "每格为 **TPS / MB/s**。读取 CRC 开启；写入 Batch=256、默认 1s 背景同步。每次写入 2GiB 总 payload，热读使用 512MiB 文件、计时 1s；以下取 3 次测量的中位数。", "",
        "| Record | 并发 | READ 随机热读 | READ 顺序热读 | WRITE 顺序持久化 |",
        "|---|---:|---:|---:|---:|",
    ]
    for size, label in ((4096, "4KiB"), (1048576, "1MiB")):
        for threads in (1, 8):
            common = dict(record_size=str(size), threads=threads)
            rand = pick(focused, mode="read", access="random", **common)
            seq = pick(focused, mode="read", access="sequential", **common)
            write = pick(focused, mode="write", batch=256, sync_ms=1000, **common)
            lines.append(f"| {label} | {threads} | {cell(rand)} | {cell(seq)} | {cell(write)} |")
    lines += ["", "## fio 存储基线", "", "每格为 **IOPS / MB/s**。4KiB 的队列深度仅为每线程 1；该结果反映请求延迟，不代表 SSD 在高队列深度下的最大 IOPS。", "",
              "| I/O | Block | 并发 | 随机读 | 顺序读 | 顺序写 |", "|---|---|---:|---:|---:|---:|"]
    for direct in (1, 0):
        for size, label in (("4k", "4KiB"), ("1M", "1MiB")):
            for threads in (1, 8):
                cells = []
                for operation in ("randread", "read", "write"):
                    row = pick(fio, direct=direct, size=size, threads=threads, operation=operation)[0]
                    cells.append(f"{row['iops']:,.0f} / {row['bytes_per_second'] / 1e6:,.1f}")
                lines.append(f"| {'Direct' if direct else 'Buffered'} | {label} | {threads} | " + " | ".join(cells) + " |")
    lines += ["", "在主要 RAF 矩阵和重复测量后，使用相同参数复测 1MiB Direct 顺序读/写，检查存储基线随时间的变化：", "",
              "| 并发 | 顺序读 GB/s（初始 / 末尾） | 顺序写 GB/s（初始 / 末尾） |", "|---|---:|---:|"]
    for threads in (1, 8):
        cells = []
        for operation in ("read", "write"):
            conditions = dict(direct=1, size="1M", threads=threads, operation=operation)
            first = pick(fio, **conditions)[0]["bytes_per_second"]
            last = pick(fio_late, **conditions)[0]["bytes_per_second"]
            cells.append(f"{first / 1e9:.3f} / {last / 1e9:.3f}")
        lines.append(f"| {threads} | " + " | ".join(cells) + " |")
    lines += ["", "基线出现时段变化；以下预设目标仍按初始基线判断，保留原始目标和未达标结果。末尾复测另用于对照持续写入。"]
    cache_diagnostic = RAW / "fio-cache-diagnostic/summary.json"
    if cache_diagnostic.exists():
        diagnostic = json.loads(cache_diagnostic.read_text())
        lines += ["", "### Buffered I/O 与热缓存的区别", "",
                  "2026-10-12 补测：维持 psync / iodepth=1 / 1MiB / 每 Job 独立 2GiB 文件 / 1s 运行 + 5s 统计，仅改变缓存策略。冷路径保留 invalidate=1；热路径先用 buffered read 完整预读所有参与文件，用 mincore 确认全部页面驻留，再使用 invalidate=0。每项单次诊断，未绑定 CPU，不替换前面的重复主结果。", "",
                  "| 并发 | 循环驱逐缓存 GB/s | 完整预热保留缓存 GB/s | 设备实际读 GB（循环驱逐 / 热缓存） |", "|---:|---:|---:|---:|"]
        for threads in (1, 8):
            cold = pick(diagnostic, threads=threads, cache="invalidate-each-loop")[0]
            hot = pick(diagnostic, threads=threads, cache="hot")[0]
            lines.append(f"| {threads} | {cold['bytes_per_second'] / 1e9:.3f} | {hot['bytes_per_second'] / 1e9:.3f} | {cold['device_read_bytes_including_ramp'] / 1e9:.3f} / {hot['device_read_bytes_including_ramp'] / 1e9:.3f} |")
        lines += ["", "设备计数来自整个 nvme0n1，观察窗口包含 fio 启动和 1s 运行阶段，热路径的完整预读在该窗口之前完成；它可能包含其他进程 I/O。各热缓存测量前的驻留页都等于文件总页，测量窗口内设备读计数没有增长。", "",
                  "fio 3.36 的 [loop_cache_invalidate 与顺序读回绕逻辑](https://github.com/axboe/fio/blob/fio-3.36/io_u.c#L315-L360) 会在 time_based 顺序读到达文件尾时再次执行缓存驱逐；普通文件通过 [POSIX_FADV_DONTNEED](https://github.com/axboe/fio/blob/fio-3.36/filesetup.c#L571-L601) 实现。因此初始表中的 10.109GB/s Buffered 读不能解释为 Page Cache 热读上限，也不能据此宣称 RAF 热读比 fio 热读快一个数量级。", "",
                  "若使用 fio 自带预热，可设置 `--direct=0 --invalidate=0 --pre_read=1`；[pre_read 文档](https://fio.readthedocs.io/en/latest/fio_doc.html#cmdoption-arg-pre-read) 明确说明完整预读会清除 invalidate 标志。", "",
                  "完整预热后仍需区分路径：fio psync 将 Page Cache 数据复制到用户缓冲；RAF mmap 直接访问映射，并执行 CRC 与 payload 消费。fio 此处是 8 个独立文件、共 16GiB；RAF 的并发热读共享一个 512MiB payload 文件，两者的缓存共享、复制和计算成本不同。", "",
                  "原始补测参数、驻留页及计数见 [fio-cache-diagnostic/summary.json](raw/fio-cache-diagnostic/summary.json)，输出见 [fio-cache-diagnostic.log](raw/fio-cache-diagnostic.log)。"]
    matched_path = RAW / "fio-mmap-matched-read/summary.json"
    reference_path = RAW / "mmap-scan-reference.json"
    if matched_path.exists() and reference_path.exists():
        matched = json.loads(matched_path.read_text())
        reference = json.loads(reference_path.read_text())
        lines += ["", "### 同文件同亲和性的 mmap 对照", "",
                  "fio 支持 mmap 引擎。fio 3.36 的 [mmap 读取实现](https://github.com/axboe/fio/blob/fio-3.36/engines/mmap.c#L214-L224) 仍将映射数据 memcpy 到 I/O 缓冲，RAF 关闭 CRC 时则在映射上直接进行完整 XOR 扫描；两者的目的缓冲写入和消费工作量不同。", "",
                  "补测统一使用同一个含 512MiB payload 的 Archive、1MiB 操作、相同大核亲和性、5s 测量窗口。单线程固定 CPU 5，8 线程固定 CPU 5/6/7/8/16/17/18/19。fio 使用 invalidate=0 / pre_read=1 / fadvise_hint=0，保留 1s 统计前运行；RAF 使用既有完整预热。所有线程读取同一文件，CRC 关闭。每项一次诊断。", "",
                  "| 线程 | fio psync GB/s | fio mmap GB/s | RAF mmap + XOR GB/s | 普通 mmap + 同样 XOR GB/s |", "|---:|---:|---:|---:|---:|"]
        for threads in (1, 8):
            cells = []
            for engine in ("psync", "mmap", "mmap+xor"):
                row = pick(matched, threads=threads, engine=engine)[0]
                cells.append(f"{row['bytes_per_second'] / 1e9:.3f}")
            row = pick(reference, threads=threads)[0]
            cells.append(f"{row['bytes_per_second'] / 1e9:.3f}")
            lines.append(f"| {threads} | " + " | ".join(cells) + " |")
        lines += ["", "fio 读取文件前 512MiB 的原始字节，RAF 统计 512MiB payload；每条 Record 的 12B 和每项 Offset 的 8B 导致约 0.002% 的覆盖差异。普通 mmap 基线从现有 benchmark 提取完全相同的 consume 函数，扫描前 512MiB，每 64 个 1MiB 块检查时间，与 RAF 相同；它没有 RAF 索引/API 操作。参考代码按相同 GCC Release 优化选项编译，并已按项目 .clang-format 格式化。", "",
                  "同文件比较的六项 fio/RAF 测量在整个进程窗口内均未观察到块设备读计数增长。普通 mmap 基线使用另一个同样生成、预热的 Archive。两组都是逻辑处理吞吐；共享缓存会减少底层实际内存读取，不能将总数直接解释为 DRAM 总线流量。", "",
                  "这些结果证明实现具备普通 mmap 扫描的吞吐量级，并展示消费方式对比较的影响。单次诊断不能量化复制、缓存行为、指令实现及 fio 记账各自的贡献，也不能据此断言 RAF 在所有 fio 引擎和负载上都更快。", "",
                  "数据：[同文件 fio/RAF](raw/fio-mmap-matched-read/summary.json)、[普通 mmap 基线](raw/mmap-scan-reference.json)、[基线源文件](raw/mmap-scan-reference.cpp)。此前共享/独立文件探索保留在 [fio-raf-matched-read/summary.json](raw/fio-raf-matched-read/summary.json)；其中部分 fio 启动阶段有设备读取，不替代本节明确预读的对照。"]
    lines += ["", "## 优化前后", "",
              "R1 为便携查表 CRC，R2 为运行时硬件 CRC；文件格式和写入架构相同。两轮均使用修正后的完整 payload 消费方式、128MiB 写预算 / 256MiB 读文件 / 0.4s 热读，单次探索测量。旧的 cache-line sampling 结果仍保留在 portable/hardware/matrix.jsonl，仅作历史记录，不用于下面结论。", "",
              "| 场景 | R1 GB/s | R2 GB/s | 倍率 |", "|---|---:|---:|---:|"]
    tags = ["crc-64-r0", "crc-4096-r0", "crc-1048576-r0", "write-4096-n1-b1-r0", "write-4096-n1-b256-r0",
            "write-1048576-n1-b256-r0", "read-4096-sequential-n1-r0", "read-1048576-sequential-n1-r0", "read-1048576-sequential-n8-r0"]
    for tag in tags:
        before = pick(portable, tag=tag)[0]["bytes_per_second"]
        after = pick(hardware, tag=tag)[0]["bytes_per_second"]
        lines.append(f"| {tag} | {before / 1e9:.3f} | {after / 1e9:.3f} | {after / before:.2f}x |")
    old_path = RAW / "before-sync-fix/sustained.jsonl"
    if old_path.exists():
        old = [json.loads(line) for line in old_path.read_text().splitlines()]
        before = pick(old, sync_ms=1000, threads=8)[0]
        after = pick(sustained, sync_ms=1000, threads=8)[0]
        lines += ["", "R3 是背景同步正确性修正：连续大 Batch 下，后台原先会等待追加 mutex；完成数改为原子发布，后台使用原有同步 mutex 独立工作。以下两次均为 8 Writer / 64GiB / 1s 背景同步。同步次数扣除最终每个文件的两次封装同步。", "",
                  "| 版本 | 实际成功背景同步次数 | 持久化 GB/s |", "|---|---:|---:|",
                  f"| 修正前 | {before['successful_syncs'] - 2 * before['threads']} | {before['bytes_per_second'] / 1e9:.3f} |",
                  f"| 修正后 | {after['successful_syncs'] - 2 * after['threads']} | {after['bytes_per_second'] / 1e9:.3f} |",
                  "", "这项修正确保设计要求的后台持久化实际执行；它不是以牺牲持久性换吞吐的优化。新加入的确定性故障注入测试也验证了背景进度及在途追加收到同步错误。"]
    lines += ["", "Batch 减少小 Record 的 syscall 次数；下表仍为 2GiB、3 次中位数，且包含最终同步。", "",
              "| Record | 并发 | Single GB/s | Batch GB/s | Batch/Single |", "|---|---:|---:|---:|---:|"]
    for size, label in ((4096, "4KiB"), (1048576, "1MiB")):
        for threads in (1, 8):
            common = dict(mode="write", record_size=str(size), threads=threads, sync_ms=1000)
            a = value(pick(focused, batch=1, **common)); b = value(pick(focused, batch=256, **common))
            lines.append(f"| {label} | {threads} | {a / 1e9:.3f} | {b / 1e9:.3f} | {b / a:.2f}x |")
    lines += ["", "## 尺寸与并发矩阵", "",
              "完整矩阵为 246 项：96 项写入（Single/Batch × 背景同步开/关 × 1/2/4/8 Writer）、120 项热读（顺序/随机 × CRC 开/关 × 1/2/4/8/16 Reader）、24 项初始冷缓存读及 6 次数据集写入。每项写入 256MiB 总 payload，读文件 512MiB，热读计时 0.4s。这组短测量用于趋势分析，封装固定成本对大量小文件影响明显；主结论采用上面的较长重复测量。混合长度按 64B/256B/4KiB/64KiB/1MiB 循环，带宽按实际字节统计。", "",
              "以下为 **顺序热读 GB/s（CRC 开 / 关）**：", "",
              "| Record | 1 线程 | 2 线程 | 4 线程 | 8 线程 | 16 线程 |", "|---|---:|---:|---:|---:|---:|"]
    for size, label in ((64, "64B"), (256, "256B"), (4096, "4KiB"), (65536, "64KiB"), (1048576, "1MiB"), ("mixed", "混合")):
        cells = []
        for threads in (1, 2, 4, 8, 16):
            common = dict(mode="read", record_size=str(size), threads=threads, access="sequential", cache="hot")
            cells.append(f"{value(pick(matrix, crc=True, **common)) / 1e9:.2f} / {value(pick(matrix, crc=False, **common)) / 1e9:.2f}")
        lines.append(f"| {label} | " + " | ".join(cells) + " |")
    lines += ["", "以下为 **Batch 持久化写 GB/s（背景同步开 / 关）**：", "",
              "| Record | 1 Writer | 2 Writer | 4 Writer | 8 Writer |", "|---|---:|---:|---:|---:|"]
    for size, label in ((64, "64B"), (256, "256B"), (4096, "4KiB"), (65536, "64KiB"), (1048576, "1MiB"), ("mixed", "混合")):
        cells = []
        for threads in (1, 2, 4, 8):
            common = dict(mode="write", record_size=str(size), threads=threads, batch=256)
            cells.append(f"{value(pick(matrix, sync_ms=1000, **common)) / 1e9:.2f} / {value(pick(matrix, sync_ms=0, **common)) / 1e9:.2f}")
        lines.append(f"| {label} | " + " | ".join(cells) + " |")
    lines += ["", "曲线使用本节短测矩阵，左图为 CRC 开启的顺序热读，右图为默认背景同步的 Batch 持久化写入；展示五种固定长度，不包含混合长度。", "",
              "![RAF 吞吐随并发与 Record 尺寸变化](performance-curves.svg)", "",
              "可下载 [SVG](performance-curves.svg) 或 [PNG](performance-curves.png)。"]
    lines += ["", "## 初始冷缓存对照", "",
              "通过文件级 POSIX_FADV_DONTNEED 请求驱逐，并用 mincore 记录计时前驻留页；不修改系统级 drop_caches。Reader 构造加载 Index 的时间单独记录。顺序执行一个遍历；随机发起与 Record Count 相同次数的有放回请求，因此后续请求可命中已缓存数据。并发 Reader 还共享其他线程读入的页。这是初始冷缓存下的逻辑处理吞吐，不是所有请求都访问物理磁盘的吞吐。", "",
              "| Record | 并发 | 冷随机 MB/s | 冷顺序 MB/s | 初始驻留页/总页（顺序） |", "|---|---:|---:|---:|---:|"]
    for size, label in ((4096, "4KiB"), (1048576, "1MiB")):
        for threads in (1, 8):
            common = dict(mode="read", record_size=str(size), threads=threads, cache="cold")
            rand = pick(matrix, access="random", **common)[0]; seq = pick(matrix, access="sequential", **common)[0]
            lines.append(f"| {label} | {threads} | {rand['bytes_per_second'] / 1e6:,.1f} | {seq['bytes_per_second'] / 1e6:,.1f} | {seq['cache_resident_before']:,}/{seq['cache_pages_before']:,} |")
    lines += ["", "## 持续写入与同步", "",
              "1MiB、Batch=256、1/8 Writer，每组 64GiB 总 payload，包含最终 Seal。每 200ms 记录写入进度、系统 Dirty/Writeback 和块设备写计数；不修改 VM 参数。当前 dirty_ratio=20%、dirty_background_ratio=10%；启动时 /proc/vmstat 的脏页阈值约为 24.5GB。", "",
              "| Writer | 背景 Sync | Payload GB | Append GB/s | 持久化 GB/s | 实际设备写 GB/s | CPU % | Dirty 峰值 GB | fdatasync 最大 ms | 成功背景 Sync 次数 |", "|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for row in sorted(sustained, key=lambda row: (row["threads"], row["sync_ms"])):
        peak = max(sample[2] for sample in row["samples"])
        lines.append(f"| {row['threads']} | {'1s' if row['sync_ms'] else '关闭'} | {row['payload_bytes'] / 1e9:.2f} | {row['append_bytes_per_second'] / 1e9:.3f} | {row['bytes_per_second'] / 1e9:.3f} | {row['storage_written_bytes'] / row['seconds'] / 1e9:.3f} | {row['cpu_percent']:.1f} | {peak / 1e9:.2f} | {row['sync_max_ns'] / 1e6:.2f} | {row['successful_syncs'] - 2 * row['threads']} |")
    max_dirty = max(sample[2] for row in sustained for sample in row["samples"])
    waits_path = RAW / "writeback-observer.jsonl"
    observations = [json.loads(line) for line in waits_path.read_text().splitlines()] if waits_path.exists() else []
    throttle = sum(row["thread_waits"].get("balance_dirty_pages", 0) for row in observations)
    evidence = f"文件之外的只读 /proc 观察器在两组 8 Writer 测量中记录到 {throttle} 次线程等待采样落在 balance_dirty_pages，直接证明本次覆盖了 Dirty Throttling 阶段；这些是采样观察次数，不是 syscall 次数或精确阻塞时长。" if throttle else "采样未直接观测到 balance_dirty_pages 等待，不能仅由总字节数断言已经覆盖 Dirty Throttling。"
    lines += ["", f"本次最高 Dirty 为 {max_dirty / 1e9:.2f}GB。{evidence}没有修改全局 VM 阈值。观察器只读取同一用户拥有的本次 RAF 大文件基准进程，证据见 [writeback-observer.jsonl](raw/writeback-observer.jsonl)。块设备计数是整台设备的观测，可能包含少量其他进程和文件系统日志写入。", "",
              "## 延迟、CPU 与索引", "",
              "下表采用主结果的 Batch 写入。延迟单位 µs，采样每 128 次 Append/Batch 调用一次；Batch 延迟是一整批的调用延迟，不能当作单 Record 的延迟。样本数较少的大 Record Case 的 P99 只能当作本次样本分位数，不能代表生产尾延迟。CPU 100% 表示一个核。", "",
              "| Record | 并发 | P50 µs | P99 µs | 每次样本数 | CPU % | fdatasync 最大 ms |", "|---|---:|---:|---:|---:|---:|---:|"]
    for size, label in ((4096, "4KiB"), (1048576, "1MiB")):
        for threads in (1, 8):
            rows = pick(focused, mode="write", record_size=str(size), threads=threads, batch=256, sync_ms=1000)
            lines.append(f"| {label} | {threads} | {value(rows, 'append_call_p50_us'):.2f} | {value(rows, 'append_call_p99_us'):.2f} | {value(rows, 'latency_samples'):,.0f} | {value(rows, 'cpu_percent'):.1f} | {value(rows, 'sync_max_ns') / 1e6:.2f} |")
    prior_focused = read("before-sync-fix/focused")
    lines += ["", "短写测量的最终同步耗时有明显波动。以下比较相同 2GiB / 1MiB / Batch=256 的三次测量；追加耗时、CPU 时间取中位数，最大 fdatasync 耗时列展示三次结果的范围。", "",
              "| 时段 | Writer | Append 秒 | CPU 秒 | 最大 fdatasync ms 范围 |", "|---|---:|---:|---:|---:|"]
    for label, measurements in (("同步修正前", prior_focused), ("最终测量", focused)):
        for threads in (1, 8):
            rows = pick(measurements, mode="write", record_size="1048576", threads=threads, batch=256, sync_ms=1000)
            sync_times = [row["sync_max_ns"] / 1e6 for row in rows]
            lines.append(f"| {label} | {threads} | {value(rows, 'append_seconds'):.3f} | {value(rows, 'cpu_seconds'):.3f} | {min(sync_times):.1f}–{max(sync_times):.1f} |")
    lines += ["", "单 Writer 的追加耗时和 CPU 时间几乎不变；8 Writer 的追加耗时有所增加，但最终 fdatasync 的增长更大。持久化吞吐下降主要对应最终同步时间增加，存储基线也出现时段变化。现有计量不能确定同步波动的具体设备或文件系统原因，因此不能据此判定原子计数带来的性能变化。", "",
              "主场景热读的 CPU 使用率如下，仍取三次中位数，100% 表示一个核。", "",
              "| Record | 并发 | 随机热读 CPU % | 顺序热读 CPU % |", "|---|---:|---:|---:|"]
    for size, label in ((4096, "4KiB"), (1048576, "1MiB")):
        for threads in (1, 8):
            common = dict(mode="read", record_size=str(size), threads=threads)
            random = value(pick(focused, access="random", **common), "cpu_percent")
            sequential = value(pick(focused, access="sequential", **common), "cpu_percent")
            lines.append(f"| {label} | {threads} | {random:.1f} | {sequential:.1f} |")
    lines += ["", "Reader 索引逻辑占用严格为 8*N 字节；Writer 的 vector 容量按需增长，保留容量可能高于逻辑长度。512MiB payload 数据集的 Index：", "",
              "| Record | Records | Index MiB | 打开耗时 ms |", "|---|---:|---:|---:|"]
    for size, label in ((64, "64B"), (256, "256B"), (4096, "4KiB"), (65536, "64KiB"), (1048576, "1MiB"), ("mixed", "混合")):
        row = pick(matrix, mode="read", record_size=str(size), threads=1, access="sequential", cache="hot", crc=True)[0]
        lines.append(f"| {label} | {row['index_bytes'] // 8:,} | {row['index_bytes'] / (1 << 20):.3f} | {row['open_seconds'] * 1000:.2f} |")
    lines += ["", "## 预先设定目标的达成情况", "",
              "在 RAF 测量前，计划设定大 Record 持久化写 ≥ 同并发 fio Direct 顺序写的 70%，热 CRC 顺序读 ≥ fio Direct 顺序读的 2 倍。以下使用主结果的三次中位数：", "",
              "| 并发 | 写目标 GB/s | 实际写 GB/s | 写是否达成 | 热读目标 GB/s | 实际热读 GB/s | 读是否达成 |", "|---|---:|---:|---|---:|---:|---|"]
    for threads in (1, 8):
        baseline_write = pick(fio, direct=1, size="1M", threads=threads, operation="write")[0]["bytes_per_second"]
        baseline_read = pick(fio, direct=1, size="1M", threads=threads, operation="read")[0]["bytes_per_second"]
        actual_write = value(pick(focused, mode="write", record_size="1048576", threads=threads, batch=256, sync_ms=1000))
        actual_read = value(pick(focused, mode="read", record_size="1048576", threads=threads, access="sequential"))
        lines.append(f"| {threads} | {baseline_write * .7 / 1e9:.3f} | {actual_write / 1e9:.3f} | {'是' if actual_write >= .7 * baseline_write else '否'} | {baseline_read * 2 / 1e9:.3f} | {actual_read / 1e9:.3f} | {'是' if actual_read >= 2 * baseline_read else '否'} |")
    lines += ["", "对持续写入目标，另外使用默认同步的 64GiB 测量，仍包含最终封装：", "",
              "| Writer | 实际 GB/s | 初始 70% 写目标 | 达成 | 占末尾 fio 写带宽 |", "|---:|---:|---:|---|---:|"]
    for threads in (1, 8):
        actual = pick(sustained, threads=threads, sync_ms=1000)[0]["bytes_per_second"]
        goal = .7 * pick(fio, direct=1, size="1M", threads=threads, operation="write")[0]["bytes_per_second"]
        last = pick(fio_late, direct=1, size="1M", threads=threads, operation="write")[0]["bytes_per_second"]
        lines.append(f"| {threads} | {actual / 1e9:.3f} | {goal / 1e9:.3f} | {'是' if actual >= goal else '否'} | {100 * actual / last:.1f}% |")
    lines += ["", "瓶颈解释：便携逐字节 CRC 原先限制大 Record；硬件 CRC 明显提升读与写。小 Record 的 Single Append 主要受 syscall 和每条元数据开销影响，Batch 聚合有利。随机小 Record 还受 Index/Payload 缓存未命中影响。热读最终包含 CRC 和全 payload 消费两次读取，大线程数的总量会受共享缓存及内存访问模式影响。写入需要 CRC、用户态到 Page Cache 的复制、页分配、文件扩展及持久化；快速 NVMe 的 Direct 覆写基线不能消除这些成本。未达到的目标如表如实保留，没有为提高数字改变指定的文件格式或 Buffered I/O 架构。", "",
              "限制：短窗口和异构核存在噪声；未测生产负载下的稳定 P99，未做真实断电或设备写缓存保证测试，没有精确统计 Dirty Throttling 的阻塞时长。CPU 使用率和设备计数是观测，不是硬件事件级瓶颈剖析。CRC 关闭仅用于比较，应按调用方的数据完整性要求选择。", "",
              "可复现命令和全部实际计数保存在每行 JSON 的 command 字段。原始数据：[初始 fio](raw/fio/summary.json)、[末尾 fio](raw/fio-final/summary.json)、[最终全矩阵](raw/matrix-final.jsonl)、[三次重复主测量](raw/focused.jsonl)、[8 Writer 持续写入](raw/sustained.jsonl)、[单 Writer 持续写入](raw/sustained-single.jsonl)、[便携 CRC](raw/portable-final.jsonl)、[硬件 CRC](raw/hardware-final.jsonl)。使用 `python3 bench/make_report.py` 重建此报告。", "",
    ]
    Path("reports/performance-report.md").write_text("\n".join(lines))
    print("Generated reports/performance-report.md from retained measurements")


if __name__ == "__main__":
    main()
