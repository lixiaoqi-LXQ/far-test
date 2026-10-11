# Record Archive Format (RAF)

Linux C++20 implementation of [design.md](design.md). No external library dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

```cpp
#include "raf/raf.hpp"
#include <vector>

raf::RecordWriter writer("events.raf");
auto id = writer.append(raf::asBytes("first"));
std::vector<raf::ByteSpan> batch{raf::asBytes("second"), raf::asBytes("")};
auto ids = writer.appendBatch(batch); // Contiguous range: first, count.
writer.sync();                      // Explicit durability boundary.
writer.seal();                      // Write footer, sync, publish header, sync.

raf::RecordReader reader("events.raf");
auto value = reader.read(id);        // CRC-checked, zero-copy payload view.
auto it = reader.iterator();
while (it.hasNext()) {
    auto record = it.next();
    // Use record.bytes() or record.stringView().
}
reader.validate();                  // Also verifies every length and length CRC.
```

The 16-byte header, 12-byte per-record overhead, and `8*N+4` footer are exactly the supplied v1 format. Integers are little endian. IDs are uint64 values starting at zero. Batch calls return a contiguous ID range. Payload length is limited to uint32. CRC32C uses runtime-selected ARM CRC instructions or x86 SSE4.2, with a portable fallback. `-DRAF_PORTABLE_CRC=ON` forces the fallback for testing and comparisons.

Each file has one writer. An append mutex serializes calls on that writer; a separate mutex serializes sync operations without blocking append during fdatasync. An atomic count publishes complete records so background sync can progress during a long or blocked append. There is no writer queue, registry, or worker for data writes. A nonblocking exclusive flock prevents another writer, reader, or recovery operation from opening the same file. Readers use shared flock; multiple readers and independent iterators are supported. Each participant opens its own fd. These locks are advisory: external programs must cooperate.

Writer creation accepts a new or zero-byte regular file, and never truncates an existing archive. An unlocked nonempty file is rejected with `InvalidArgument`; a conflicting lock returns `Busy` first. Completed archives cannot be reopened for append. `seal()` and `close()` are equivalent and idempotent after success. Concurrent seal calls may return `Closed` while one call is sealing; append/sync also fail once sealing begins.

An append success acknowledges a complete record in the page cache. It does not promise durability. The default background sync interval is one second, and `WriterOptions{std::chrono::milliseconds{0}}` disables periodic sync. A successful sync acknowledges only the record count captured before its syscall began. `syncStatus()` exposes that watermark, successful sync count, timing, and the sticky I/O error; it waits for any active sync to finish. Empty and already-synchronized writers skip explicit/periodic sync. Seal always synchronizes the footer before updating and synchronizing the header. Actual sync completion may take longer than the configured interval. A background sync failure also reaches an in-flight append; its already-entered syscall can still finish records, which recovery may retain.

Call `seal()` or `close()` explicitly and handle its error. Destruction stops background sync and releases resources; it leaves unfinished files unsealed and does not hide seal errors. Write/sync/seal I/O failures are sticky: future writes fail and the archive needs explicit recovery after releasing the writer. A failed batch may have written a complete prefix even though the call threw; `recordCount()` reports only complete records.

Readers accept structurally valid sealed files. Opening checks the header, footer CRC, and offset ranges; normal reads check payload CRC and use index-derived lengths. `validate()` also checks stored lengths and length CRCs. `ReaderOptions{false}` disables payload checking on normal reads for measurement or explicitly trusted data, while `validate()` still checks all CRCs. Views and iterators must not outlive their reader. Separate iterators can run on different threads; a single iterator's cursor is not shared. Object destruction must not race with API calls.

Recovery is explicit and modifies the archive in place:

```cpp
auto result = raf::recover("interrupted.raf");
// result.records, valid_data_end, original_file_size
```

It takes an exclusive flock, streams payloads through a 64KiB buffer, stops at the first invalid/incomplete record, truncates the tail, and reseals with the required sync order. An actual pread I/O error aborts before truncation. An unrecognizable version or a header shorter than 16 bytes is `Unrecoverable`; a bad header CRC with a recognizable version is scanned from byte 16. A recovery write/sync failure is reported and can be retried after the OS/storage error is resolved.

`raf::Error` carries an `ErrorCode` and, for syscall errors, `systemError()`. Ordinary opens never perform recovery. Format boundaries remain as documented in the design: a single damaged header can be unrecoverable; without a footer marker, scanning after a bad header CRC is probabilistic; CRC is not tamper protection. File sync does not sync a newly created parent-directory entry. Use parent-directory fsync externally if filename persistence across power loss is required.

Standalone benchmark commands:

```sh
python3 bench/run_fio.py --runtime 5 --size 2G
python3 bench/run_bench.py --suite smoke --output reports/raw/smoke.jsonl
python3 bench/run_bench.py --suite matrix --output reports/raw/raf.jsonl
python3 bench/run_bench.py --suite sustained --output reports/raw/sustained.jsonl
python3 bench/run_bench.py --suite sustained --sustained-threads 1 --output reports/raw/sustained-single.jsonl
```

Benchmarks create and remove only their own scratch files in the current directory. Matrix writes default to 256MiB of total payload per case, read files to 512MiB, and warm read timings to 0.4s. Sustained writes use 64GiB total per case. Use `--device` for the backing block device, and run other heavy workloads separately for comparable results. The RAF library always uses buffered I/O; direct I/O is used only by fio for the storage baseline.

The retained measurements reproduce the report with `python3 bench/make_report.py`. Export the optional throughput curves with `uv run --with matplotlib --no-project python bench/plot_results.py`; matplotlib is only needed for plotting.

See [test report](reports/test-report.md), [performance report](reports/performance-report.md), and [implementation report](reports/implementation-report.md) for actual results and limitations.

中文实现细节与使用注意事项见 [实现说明](docs/implementation-notes.md)，包括读取路径、短读短写、IOV_MAX、索引更新、同步顺序和故障恢复边界。
