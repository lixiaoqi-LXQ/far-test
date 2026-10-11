# RAF 实现方案

## 1. 目标

实现 Append-only 的变长 Record 文件格式 RAF。

性能目标：

- 写：持续吞吐尽量接近裸盘写入带宽。
- 读：Page Cache 预热后，多线程顺序遍历同一文件，尽量达到内存侧吞吐上限。
- 小 Record：避免不必要的系统调用和内存复制。
- Burst：利用 Linux Page Cache 缓冲突发写入，不额外实现用户态缓冲队列。

功能目标：

- 单条及 Batch Append。
- 按 Record ID 随机读取。
- 多个 Iterator 并发顺序遍历。
- Record 和文件索引 CRC 校验。
- 故障后恢复最长有效 Record 前缀。
- 明确持久化及文件并发语义。

实现环境：Linux、C++、Buffered I/O。

## 2. 文件格式

### 2.1 整体布局

```text
+-------------+------------------------+-----------------+
| Header      | Records                | Footer          |
+-------------+------------------------+-----------------+
| 16B         | Variable               | 8*N + 4B        |
+-------------+------------------------+-----------------+
```

所有整数使用 Little Endian。CRC 统一使用 CRC32C。

**Header（16B）**

| 字段 | 大小 |
|---|---|
| version | 4B |
| footer_offset | 8B |
| header_crc | 4B |

- `footer_offset = 0`：文件尚未 Seal。
- `footer_offset > 0`：指向 Footer 起始位置。
- Header CRC 覆盖前 12B。
- version 当前固定为 1。

**Record（变长）**

```text
[len:4B][len_crc:4B][payload:len][payload_crc:4B]
```

- len 表示 Payload 长度。
- len_crc 校验 len 字段。
- payload_crc 校验 Payload。
- 每条 Record 固定开销 12B。
- Record ID 从 0 开始，等于物理追加顺序。
- 支持零长度 Record。
- 最大 Payload 长度由 32-bit len 限制。

**Footer（8*N + 4B）**

```text
[offset_0:u64] ... [offset_N-1:u64] [crc32c:u32]
```

- 保存 N 个 Record 的起始文件偏移。
- Footer CRC 覆盖整个 Offset Array。
- Footer 起始位置由 Header.footer_offset 提供。
- Record 数量通过 `(file_size - footer_offset - 4) / 8` 计算。
- N=0 时 Footer 仅包含 4B CRC。

不增加额外 Footer 元数据。

### 2.2 Offset 计算

对于 Record i：

```text
begin = offsets[i]
end   = offsets[i+1]  // i < N-1
end   = footer_offset // i == N-1

payload_len = end - begin - 12
payload_pos = begin + 8
payload_crc_pos = end - 4
```

Reader 可以通过 Index 直接得到 Payload 位置和长度。

## 3. Writer

### 3.1 写入架构

```text
Application Thread
        |
        v
   append(record)
        |
        v
   write / writev
        |
        v
    Page Cache
        |
        v
   Disk Writeback

Background Thread
        |
        v
   fdatasync(1s)
```

不引入 Queue、Worker 写入线程或其他用户态缓冲层。

### 3.2 Append

接口：

```cpp
RecordId append(ByteSpan payload);

RecordRange appendBatch(
    Span<ByteSpan> records
);
```

实现要求：

1. Writer 持有独立 fd 和当前写入位置。
2. Append 直接通过 `writev` 将 Record 写入 Page Cache。
3. 单条 Record 可由 len、len_crc、payload、payload_crc 四段 iovec 组成。
4. Batch 将多条 Record 聚合到尽可能少的 writev 调用中。
5. 正确处理 IOV_MAX、EINTR、short write 和其他 I/O 错误。
6. 完整写入一条 Record 后，才将它的起始位置追加到内存 Offset Array。
7. 同一个 Writer 内部使用 Mutex 串行化 Append，避免交错写入。

使用顺序 write/writev 配合 pwrite 更新 Header，不依赖 O_APPEND。

Write 发生不可恢复错误时，停止继续 Append，返回错误，由后续 Recovery 处理未完成的文件。

Batch 不承诺崩溃原子性，故障后可能只保留部分完整 Records。

### 3.3 持久化

后台线程每隔 1s 执行：

```cpp
fdatasync(fd);
```

要求：

- 只对存在未同步写入的文件执行 Sync。
- 不持有 Append Mutex 等待 fdatasync 完成。
- 记录每次 Sync 的成功状态和持久化水位。
- fdatasync 失败必须反馈给 Writer，不能静默忽略。

持久性语义：

- `append()` 返回成功：Record 已完整写入 Page Cache，不保证持久化。
- 后台 fdatasync 成功：对应同步水位之前的 Records 已确认持久化。
- `sync()`：主动执行持久化并等待完成。
- `seal()`：完成文件封装并持久化。

Sync 水位使用 Record Count 表示，并取同步开始前已经完成写入的 Record 数，成功后才能更新。

**默认 1s 同步间隔是持久化策略，不是严格的 1s 数据损失保证。**

故障后可能丢失最后一次成功 Sync 之后的 Records，也可能保留其中一部分。

### 3.4 Seal

Seal 负责结束追加并生成 Footer。

流程：

1. 禁止新的 Append，等待正在执行的 Append 结束。
2. 停止后台 Sync，确认没有并发封装操作。
3. 将内存 Offset Array 写入文件末尾。
4. 计算并写入 Footer CRC。
5. 执行 fdatasync，确保 Records 和 Footer 持久化。
6. 更新 Header.footer_offset 和 Header CRC，使用 pwrite。
7. 再执行 fdatasync，持久化 Header。
8. Seal 成功后释放 Writer。

必须严格遵守 Footer 先持久化、Header 后更新的顺序。

Seal 失败时返回错误，文件交由 Recovery 处理。

完成 Seal 后，不支持再次打开 Writer 继续追加。

## 4. Reader

### 4.1 初始化

1. 打开文件，获取 flock 共享锁。
2. 读取 Header，校验版本及 CRC。
3. 通过 footer_offset 定位 Footer。
4. 从文件大小推导 Record Count。
5. 加载 Offset Array 到内存。
6. 校验 Footer CRC、Offset 范围和单调性。

正常 Reader 只接受完成 Seal 且结构有效的文件。

对于未 Seal、Header/Footer 错误的文件，返回需要 Recovery 的状态。

### 4.2 数据读取

建议优先使用 `mmap(MAP_SHARED, PROT_READ)` 映射文件。

理由：

- 热读直接利用 Page Cache。
- 不需要逐条 pread。
- 不需要额外复制 Payload。
- 多线程可以共享同一份映射。

提供：

```cpp
RecordView read(RecordId id);

Iterator iterator();
```

RecordView 提供 Payload 指针和长度，其生命周期不能超过 Reader。

常规读取流程：

1. Index 定位 Record。
2. 根据相邻 Offset 计算 Payload 长度。
3. 定位 Payload 和 Payload CRC。
4. 校验 Payload CRC。
5. 返回 RecordView。

常规热读路径不需要单独读取和校验 len；完整扫描校验时才验证 len_crc。

### 4.3 Iterator

每个 Iterator 有独立的当前 Record ID：

```cpp
auto it = reader.iterator();

while (it.hasNext()) {
    RecordView record = it.next();
    // process record
}
```

多个 Iterator 可以在不同线程中同时遍历同一文件。

- 共享只读 Index。
- 共享 mmap 映射。
- 不共享 Iterator 状态。
- 不需要每条 Record 进行系统调用。

Reader 本身应允许多个线程安全地创建和使用独立 Iterator。

建议上层线程共享一个 Reader 实例，避免重复加载 Index，不需要额外实现全局 Index Cache。

## 5. Recovery

### 5.1 目标

故障后保留从 Record 0 开始的最长连续有效前缀。

遇到第一条损坏或不完整的 Record 即停止，不尝试跳过损坏位置。

### 5.2 恢复流程

```text
Read Header
     |
     v
Scan Record 0
     |
     v
Check len_crc
     |
     v
Check len / file boundary
     |
     v
Check payload_crc
     |
     +--- Valid ---> Save offset ---> Next Record
     |
     +--- Invalid ------------------> Stop
                                        |
                                        v
                                   Truncate tail
                                        |
                                        v
                                    Rebuild Footer
                                        |
                                        v
                                     Seal file
```

扫描时：

1. 从固定 Header 末尾开始。
2. 读取 len、len_crc。
3. 校验 Length，检查 Record 是否超出文件边界。
4. 流式计算 Payload CRC，避免为单条大 Record 分配大量内存。
5. CRC 正确则记录 Offset。
6. 遇到错误、EOF 或不完整 Record 时停止。

若 Header 有效且提供 footer_offset，可将其作为 Records 的结束边界。

若 Header CRC 无效，但 version 仍可识别，则按照固定 Header 大小从 Records 起点扫描。

若 Header 无法识别，返回 Unrecoverable。

由于不增加 Footer 标记，Header 无效时依赖 Record CRC 和边界检查判断 Records 结束；这是当前简化格式的恢复边界。

### 5.3 重建

Recovery 必须持有文件独占锁。

1. `ftruncate` 到最后一条有效 Record 的结束位置。
2. 根据扫描生成的 Offset Array 重写 Footer。
3. 执行 fdatasync。
4. 更新 Header.footer_offset 和 CRC。
5. 再执行 fdatasync。

恢复完成后，该文件成为正常 Sealed RAF 文件。

Recovery 显式调用，不在普通 Reader 打开时自动修改文件。

文件中间发生损坏时，也只保留此前有效的前缀；这不意味着中间损坏之后的数据一定不可修复，而是 RAF 不尝试恢复非连续数据。

## 6. 并发语义

采用 flock：

| 场景 | 规则 |
|---|---|
| 多 Reader，同文件 | 允许 |
| 多 Writer，同文件 | 禁止 |
| Reader + Writer，同文件 | 禁止 |
| 多 Writer，不同文件 | 允许 |
| Recovery + 任意访问，同文件 | 禁止 |

Writer 和 Recovery 获取 `LOCK_EX`。

Reader 获取 `LOCK_SH`。

默认采用非阻塞获取锁，冲突时返回 Busy。

每次打开文件使用独立 open fd，不能通过 dup 出来的 fd 实现彼此独立的锁。

Writer 的后台 Sync 线程可以共享所属 Writer 的 fd。

## 7. 测试

### 7.1 正确性

必须覆盖：

- 0、1、大量 Records。
- 固定长度和变长 Records。
- 零长度和大 Payload。
- 单条 Append、Batch Append。
- 顺序读取、随机读取、多 Iterator。
- Header CRC 错误。
- Footer CRC 错误。
- Record Length / Payload 损坏。
- 文件尾部任意位置截断。
- Append 中途失败。
- Seal 各阶段失败。
- 多 Reader / Writer 锁冲突。
- Recovery 后所有保留 Records 能完整读取并校验。

通过截断和修改字节模拟数据损坏，检查恢复结果是否为预期有效前缀。

另外需要注入 write、fdatasync 错误，不能仅依赖 kill -9 模拟所有崩溃情况。

### 7.2 写入 Benchmark

Record Sizes：

`64B / 256B / 4KiB / 64KiB / 1MiB / 混合长度`

测试：

- Single Append vs Batch Append。
- 1/2/4/8 线程写不同文件。
- 后台 fdatasync 开启与关闭。
- 持续写入至少覆盖 Page Cache 进入 Dirty Throttling 的阶段。

测量：

- 持续写入带宽（GB/s）。
- Append P50/P99。
- fdatasync 耗时。
- CPU 使用率。
- 磁盘实际写入带宽。

同时报告包含最终 Seal/Sync 时间的端到端持久化吞吐。

### 7.3 读取 Benchmark

Record Sizes 与写入测试一致。

测试：

- 1/2/4/8/16 线程，同时遍历同一文件。
- 热 Page Cache 为重点，同时提供冷读对照。
- CRC 开启与关闭的性能差异。
- 随 Record 大小变化的吞吐曲线。

测量：

- 总吞吐（GB/s）。
- Records/s。
- CPU 使用率。
- Index 内存占用。

对热缓存读取，重点分析 CRC 计算与内存带宽瓶颈。

## 8. 实现边界

本版本不实现：

- 双 Header、额外 Footer Magic、Trailer。
- Block 分组、压缩或分层索引。
- 用户态异步写入 Queue。
- io_uring / O_DIRECT。
- 多 Writer 同文件写入。
- 未 Seal 文件的在线读取。
- 数据损坏后跳过错误 Record 继续读取。
- 自动 Recovery 或复杂事务机制。

需要明确的限制：

1. 单 Header 原地更新无法保证在任意 Header 物理损坏时恢复；CRC 可以检测部分损坏，但无法修复不可识别的 Header。
2. 无 Footer Marker 时，Header 失效后的扫描结束判定依赖 Record CRC，属于高概率识别而非绝对无歧义。
3. CRC32C 不保证检测所有可能的数据篡改或 CRC 碰撞。
4. `fdatasync(fd)` 不保证新建文件的目录项已经持久化；如果要求断电后文件名也必须存在，需要在创建阶段同步父目录。
5. 持续高负载下，Linux Dirty Throttling 可能使同步 Append 阻塞；本版本接受这种背压。

## 9. Agent 交付要求

请严格按照以上设计实现，不自行扩展文件格式或新增架构层。

交付：

1. C++ RAF Writer、Reader、Iterator、Recovery。
2. 完整单元测试和故障注入测试。
3. 独立 Benchmark。
4. 简明的实现报告，包括实际性能、瓶颈和没有解决的问题。

实现时先保证文件格式、校验、持久化与恢复正确，再进行性能优化。

如发现必须改变文件格式才能解决的正确性问题，先报告设计冲突，不要自行增加 Header、Footer 字段或新的存储结构。