# RAF 实现细节与使用注意事项

本文对照 `21c6e2e` 中的实际实现整理，适用于当前 Linux C++20 版本。主要依据是 [公开接口](../include/raf/raf.hpp)、[读写与恢复实现](../src/raf.cpp)、[CRC 实现](../src/crc32c.cpp) 和 [故障注入测试](../tests/raf_tests.cpp)。下面区分已经实现的保护与仍存在的边界，不将设计目标视为已验证行为。

## 1. 文件布局与核心状态

所有整数使用小端编码，记录 ID 从 0 开始。

| 区域 | 内容 | 大小 |
|---|---|---|
| Header | version(u32)、footer offset(u64)、前 12 字节的 CRC32C(u32) | 16B |
| Record | payload length(u32)、length CRC32C(u32)、payload、payload CRC32C(u32) | payload + 12B |
| Footer | 每条记录起点 offset(u64) 数组、数组 CRC32C(u32) | 8N + 4B |

空 payload 也是一条完整记录，有长度字段和两份 CRC；空 archive 封装后是 20B。Writer 初始 Header 的 footer offset 为 0，表示尚未封装。

Writer 的几个量不能混用：

- `position`：实际成功写入的记录区字节末端；短写后可能位于半条记录中。
- `offsets`：已经完整写入记录的起点数组；完整指包括尾部 payload CRC 写完。
- `completed` / `recordCount()`：通过原子变量发布的完整记录数。
- `durable_records`：成功 `fdatasync` 所确认的保守记录数，不等于当前 `recordCount()`。

内存中的索引在 append 时更新，磁盘 Footer 到 `seal()` 才写入。因此 `sync()` 成功的未封装文件仍需显式恢复后才能用 Reader 打开。

## 2. 读取如何工作

### 2.1 打开：先验证索引，再建立映射

`RecordReader` 打开只读 fd、取得非阻塞共享 `flock`，然后：

1. 用 `fstat` 检查普通文件、文件大小与地址空间限制。
2. 用 `readAt()` / `pread` 读取 Header，检查版本及 Header CRC。
3. 检查 footer offset 范围、Footer 长度是否为 `8N+4`，并根据记录最小开销判断数量是否可能。
4. 将索引加载到 `vector<uint64_t>`，校验原始小端字节的 Footer CRC，再进行必要的端序转换。
5. 检查首记录从 byte 16 开始、相邻 offset 严格递增、每条记录至少 12B、推导 payload 不超过 `UINT32_MAX`。
6. 对整个文件建立 `PROT_READ | MAP_SHARED` 映射。

打开不会自动修复文件，也不会扫描全部 payload。结构异常通常返回 `NeedsRecovery`；真实 syscall 错误返回 `Io`。打开失败由 RAII 释放 fd 和锁。

### 2.2 热路径：相邻 offset 决定长度

`read(id)` 先检查 ID，然后取 `begin=offsets[id]`，`end=offsets[id+1]`；最后一条使用 Footer 起点作为 `end`。返回：

```text
payload pointer = mapping + begin + 8
payload length  = end - begin - 12
payload CRC     = end - 4 处的 u32
```

默认计算 payload CRC，失败返回 `CorruptRecord`。这个路径直接访问映射，不为每条记录调用 `pread`，也不复制 payload。但 CRC 校验会读取全部 payload，缺页仍可能引起实际磁盘 I/O；“零拷贝”不代表“没有 I/O”或“不读取 payload”。

普通 `read()` 不检查记录中的 length 和 length CRC，也不依赖该 length 定位下一条。`validate()` 才逐条检查 length CRC、存储长度与索引推导长度是否相等，以及 payload CRC。`ReaderOptions{false}` 只关闭普通读取的 payload 校验，不能关闭 `validate()` 的完整检查。

### 2.3 视图与并发的约束

`RecordView` 引用 Reader 的映射，Reader 析构后不能继续使用；要长期保存就复制到自己的缓冲。`Iterator` 也引用 Reader，必须服从相同生命周期。多个线程可以共享只读 Reader 并使用各自的 Iterator；同一个 Iterator 的游标不能无同步地共享。销毁 Reader 不得与读取并发。

`flock` 是协作锁，不能阻止忽略锁的其他程序修改或截断文件。映射建立后不能假定任意外部变更都变成 `raf::Error`；例如访问截断后的映射区域可能触发进程级 `SIGBUS`。Reader 存活期间应保持文件内容不变。

## 3. 短读、短写与 EINTR

写入路径需要处理的是短写；短读出现在 Reader 初始化与 Recovery 的 `pread` 路径。正数返回值仅说明本次完成了多少字节，不能据此认为整个请求完成。

| 函数 | 正数但不足请求长度 | `-1/EINTR` | 返回 0 | 其他错误 |
|---|---|---|---|---|
| `readAt` / `pread` | 推进缓冲指针和显式 offset，继续读取 | 原位置重试 | 返回 false，表示遇到 EOF | 抛 `Io` |
| `writeAll` / `write` | 消耗已写的 span 前缀，继续写 | 重试 | 抛 `Io(EIO)` | 抛 `Io` |
| `writeAt` / `pwrite` | 推进 span 和显式 offset | 重试 | 抛 `Io(EIO)` | 抛 `Io` |
| append 的 `writev` | 推进文件位置、完整记录计数及 iovec 游标 | 不推进状态，重试 | 抛 `Io(EIO)` 并使 Writer 失败 | 同左，保留原始 errno |

`readAt()` 和 `writeAll()` 将单次请求限制为 1GiB；`writeAt()` 没有相同限制，当前用途仅是 16B Header。`fdatasync`、`flock` 和恢复中的 `ftruncate` 也重试 `EINTR`；不要据此推断所有 syscall 都实现了 EINTR 重试。

Recovery 特别区分 EOF/校验失败与真实读错误：前者表示可以保留完整前缀；`pread` 的 `EIO` 等错误直接抛出，发生在截断之前，避免将暂时读不到的数据当作损坏删除。

## 4. writev、IOV_MAX 与批量组织

### 4.1 为什么每条记录最多三个 iovec

每条记录组织为：8B length+length CRC、payload、4B payload CRC。空 payload 省略中间段，只使用两个 iovec。元数据保存在当前 chunk 的栈上数组中，payload 直接引用调用者的 `ByteSpan`。

每个 chunk 最多 256 条，iovec 数组容量为 768。运行时通过 `sysconf(_SC_IOV_MAX)` 查询上限，查询不到正值时回退到 1024：

```text
chunk_records = min(256, max(1, IOV_MAX / 3))
```

因此在本机 `IOV_MAX=1024` 时，一次最多组织 256 条非空记录、768 个 iovec；如果系统上限为 16，则每个 chunk 最多 5 条非空记录。4097 条的批量由循环拆成多个 chunk，不能把所有记录一次传给 `writev`。系统对向量数量及总长度有各自限制，见 [Linux writev 手册](https://man7.org/linux/man-pages/man2/writev.2.html)。

当前公式在假设上限至少为 3 时满足数量限制；未知值回退 1024 和上限小于 3 的情形不是通用平台保证。当前目标是 Linux，不能直接把这段逻辑宣称为任意平台可移植实现。

### 4.2 短写可能落在任意段中间

`writev` 返回正数 `written` 后，按顺序执行：

1. `position += written`。
2. 将满足 `ends[i] <= position` 的完整记录起点加入索引，并发布 `completed`。
3. 用 `written` 消耗 iovec：写完的段跳过；写了一部分的段将 `iov_base` 向后移动、`iov_len` 减小。
4. 下一次只提交未写完的后缀，直到当前 chunk 全部完成。

例如 payload 为 `hello` 的记录共 17B。若第一次只写 2B，第二次写 7B，就会先补完 Header，再进入 payload；不能重新提交整条记录，否则重复字节会破坏文件布局。栈上的元数据数组在当前 chunk 完成前不会被复用。

调用者必须保证所有 payload 在整个 append 调用期间有效且不被修改。CRC 在组织 chunk 时计算，随后 writev 使用原内存；并发改动 payload 可能造成 CRC 与实际字节不一致。

### 4.3 数量限制不等于总字节限制

目前代码没有为每次 `writev` 增加独立的总字节预算。Linux 单次传输上限造成的正数短写可由现有循环继续处理；但若 iovec 长度之和超过 `ssize_t` 可表示范围，syscall 可返回 `EINVAL`，并触发粘滞错误。不能将“每条记录允许 uint32 长度”理解为“任意平台上接近 4GiB 的真实写入已经验证”。

未来扩展到更窄的平台或需要明确限制大请求时，应同时按 iovec 数量和总字节数拆分，必要时拆分单条 payload。当前大 payload 实际测试是 MiB 级，未覆盖接近 `UINT32_MAX` 的真实写入。

## 5. 避免写入失败后索引失真

### 5.1 先验证、先分配，后写入

`appendBatch()` 持有追加 mutex，先检查 Writer 状态，再遍历整个 batch 检查 payload 长度、记录区 `off_t` 范围、记录数量溢出，以及最终 Footer 的空间范围。任意输入不合法都在写入前失败。

随后为整个 batch 的最终索引数量执行 `offsets.reserve()`，按至少 256、容量约倍增的策略扩容。这一步在实际 writev 之前，故 `bad_alloc` 或容量异常不会发生在“数据已写完，但登记索引还需要扩容”的窗口。后续 `push_back(uint64_t)` 在预留容量内不需要重新分配。

这不是全项目的无内存失败保证：Reader 要分配完整索引，Recovery 也会增长索引数组，标准库异常并未统一包装为 `raf::Error`。

### 5.2 完整记录才发布索引

chunk 预先计算每条记录的 `starts` 和 `ends`，但不提前加入 `offsets`。只有写入位置越过完整记录的 `end`，才登记其 `start`。这同时避免两类错误：

- payload 写完而尾部 CRC 没写完，却把半条记录算作成功。
- 整个 batch 尚未写完，就把所有计划 offset 提前加入索引。

例：`hello`、`world` 各为 17B。首个 syscall 写入 25B，意味着第一条完整、第二条只写了 8B Header。随后 `ENOSPC` 时，`recordCount()` 必须为 1；Recovery 只能保留第一条。这正是 `short_batch_error_publishes_only_complete_records` 的测试场景。

### 5.3 写入失败后不继续追加

writev、同步或封装 I/O 失败会记录第一个错误，停止后台同步，并让后续 append/sync/seal 返回 `Io`。不能在原 Writer 上简单重试 batch：当前文件末端可能在半条记录中，重试会把新记录接在坏尾巴后。

失败 batch 不保证全有或全无。可能已经写入完整前缀，甚至后台同步失败时，已进入内核的在途 writev 仍会完成更多记录；append 会在后续检查中抛错，但 `recordCount()` 仍反映这些完整记录。调用者应释放 Writer、显式 `recover()`、重新读取实际保留内容，再决定业务重试策略。RAF 没有业务去重或事务回滚功能。

## 6. 并发、同步与封装顺序

同一 Writer 的 append 用 `mutex` 串行化；数据 syscall 在调用线程执行，没有异步写队列。另一把 `sync_mutex` 串行化同步和统计，`fdatasync` 期间不持有追加锁。因此同步等待磁盘时追加可以继续。

`completed` 用 release store 发布，后台用 acquire load 获取完整记录数，无需等待正在进行的大 batch 的追加锁。后台同步先捕获 target，再调用 `fdatasync`，成功后才设置 `durable_records=target`；即使期间又追加了更多记录，也不夸大水位。默认同步间隔是 1000ms，0 禁用后台同步；定时间隔不是持久化完成时间保证。

没有新增完整记录时 explicit/periodic sync 跳过 syscall；`syncStatus()` 会等待正在执行的同步结束。业务若要明确持久化边界，应先完成相应 append，再调用 sync；若还需要可直接打开的 archive，则调用 seal。

`seal()` 的顺序是：

```text
Open -> Sealing（阻止新 append/sync）
停止并等待后台线程
写 Footer 的 offset 数组及 CRC
fdatasync（数据和 Footer）
pwrite Header（发布 Footer 起点与 Header CRC）
fdatasync（Header）
Sealed，关闭 fd 并释放锁
```

先同步 Footer 再发布 Header，防止先把一个未完整落盘的索引标记为可读取。任一步失败都不能宣称封装成功；Header 写入不是原子事务，撕裂仍依赖 CRC 检测与恢复。成功 seal/close 可重复调用；并发 seal 期间其他 seal 可能得到 `Closed`。

析构只停止后台并释放资源，不替调用者 seal，也不报告封装错误。应显式调用 `seal()` / `close()` 并处理异常。新文件名的断电持久化还需要调用者处理父目录 fsync；当前实现只同步文件。

## 7. 恢复流程与恢复边界

`recover()` 取得非阻塞独占锁；必须先销毁持锁的 Writer/Reader。它不支持恢复后在原文件继续追加，Writer 仍仅接受新文件或零字节文件。

扫描从 byte 16 开始。Header 版本必须可识别；不足 16B 或版本不对返回 `Unrecoverable`。Header CRC 有效且 footer offset 至少为 16 时，以它与原始文件大小的较小值限定扫描边界；Header CRC 无效时不能信任 footer offset，扫描边界使用原文件大小。

每条记录先验证 length CRC 和边界，再通过固定 64KiB 缓冲流式验证 payload CRC。只有整条有效才登记 offset；遇到第一条损坏/不完整记录立即停止，不跳过损坏去寻找后续记录。扫描结束后截断尾部，重写 Footer，再按“同步 Footer、写 Header、同步 Header”封装。

恢复扫描阶段的真实读错误发生在文件修改前；但一旦进入 truncate/write/sync 阶段就不是事务，失败可能已经修改文件。故障消除后可重试，现有测试覆盖多个恢复修改阶段的失败与重试。

格式只有一份 Header，没有独立 Footer marker。Header 的版本本身损坏可能无法恢复；Header CRC 损坏时，全文件扫描有把 Footer 字节偶然解释成合法记录的概率，CRC 不提供数学上的零误判保证或防篡改能力。重要数据的破坏性恢复前应保留原文件副本。

## 8. 已有证据与尚未验证项

| 关注点 | 对应测试名称（`tests/raf_tests.cpp`） |
|---|---|
| 超过一次 iovec 上限的 batch | `batch_spans_more_than_iov_max` |
| EINTR、段内及跨段短写 | `writev_eintr_and_partial_iov_are_retried` |
| ENOSPC 后仅发布完整前缀、零写入失败 | `short_batch_error_publishes_only_complete_records`、`zero_write_is_fatal_and_recoverable` |
| pread 短读重试、真实读错误不截断 | `pread_short_and_eintr_are_retried`、`recovery_io_failure_does_not_truncate_valid_file` |
| 同步不阻塞追加、不夸大水位 | `sync_does_not_block_append_or_overstate_watermark` |
| batch 阻塞时后台进度与在途失败 | `background_sync_progresses_while_append_is_blocked`、`background_sync_error_reaches_inflight_append` |
| 封装顺序、各阶段故障与撕裂 Header | `seal_footer_before_header_sync_order`、`seal_failures_at_each_stage_recover`、`torn_header_and_short_footer_are_recoverable` |
| 恢复修改失败后重试、任意尾截断 | `recovery_mutation_failures_can_be_retried`、`every_truncation_position_recovers_longest_prefix` |
| 普通读与完整校验的差别 | `length_corruption_is_detected_by_full_scan`、`crc_disabled_reader_still_full_validates` |

故障注入用测试程序的链接器 `--wrap` 包装 syscall，大部分操作仍在真实临时文件上运行；它验证错误处理路径，不等价于硬件断电或真实坏盘实验。历史验证及限制见 [测试报告](../reports/test-report.md)。

本次文档核对重新执行了 Release 构建、41 项核心用例及 `benchmark_smoke`（CTest 为两个测试程序）。没有新增实现或测试；未覆盖的重点仍包括真实断电、非协作截断 mmap、接近 4GiB payload、索引内存耗尽、x86/大端实际运行。历史 TSan 在本机因 `unexpected memory mapping` 无法启动，不能声称动态数据竞争验证通过。

维护时应保持四个不变量：**写入前验证并预留索引容量；完整记录后才发布 offset；I/O 失败后停止继续追加；Footer 同步成功后才发布并同步 Header。** 修改批量大小或 syscall 包装时，须同时检查 iovec 数量、字节数、短写游标及失败前缀语义。
