# RAF 实现报告

实现保持原设计的单文件布局和同步写入模型，生产代码集中于 `src/raf.cpp` 与 `src/crc32c.cpp`，公开 API 在 `include/raf/`。使用 C++20 标准库和 Linux syscall，无第三方运行时依赖。

Writer 使用 writev 直接追加，没有队列或数据写入线程。Length 与 Length CRC 合并在一个 8B 元数据段，Payload 和 Payload CRC 为另外两段；每次最多组织 256 条 Record，并按系统 IOV_MAX 限制分块。short write 后原位调整 iovec，只有整条 Record 写完才登记 Offset。Batch 故障保留已完成前缀并使 Writer 进入粘滞错误状态。

每个文件只有一个 Writer。同一 Writer 的追加用一个 mutex 串行化；另一个 mutex 串行化同步并保护同步统计，fdatasync 不持有追加 mutex。完成数使用原子计数发布，后台在大 Batch 或阻塞 writev 期间也能取得保守同步水位。停止标志和粘滞错误同样用少量原子状态传递，没有增加锁层。后台线程仅负责默认每秒的脏数据同步。打开文件时取得非阻塞 flock，不做 Writer 注册表、锁升级或复杂的多 Writer 生命周期管理。构造时先取得锁再初始化，不使用 O_TRUNC。

Reader 加载并校验只读 Offset Array，然后 mmap 整个文件。常规热读由相邻 Offset 推导 Length，校验 Payload CRC 后返回零拷贝视图；完整 validate 还校验 Length 和 Length CRC。每个 Iterator 只有独立的 Record ID，没有热路径 syscall 或共享游标锁。

Recovery 取得独占锁，用 64KiB 固定缓冲流式扫描，遇到首条损坏或不完整 Record 就停止。真实 pread I/O 错误直接返回，避免把暂时无法读取的数据当成损坏并截断。重建时与 Writer 一样，先同步 Footer，再更新并同步 Header。没有自动恢复、跳过损坏位置或文件格式扩展。

唯一算法性能优化是运行时分派 CRC32C：ARM CRC 指令、x86 SSE4.2，以及便携查表后备。保留便携构建及其测量记录，使用同样数据和基准比较。Batch 与 Single 的差异也单独测量。读取基准最终采用全 payload 字节 XOR 消费，保证 CRC 关闭时计量仍对应实际读取。

用户澄清和执行判断：按已有 design.md 直接实现；当前目录没有 Git，因此没有新建仓库或提交。接口采用设计中的 ByteSpan/RecordRange 和 uint64 ID。析构仅释放资源，成功结束必须显式调用 seal/close，使封装错误能被调用者处理。原始 syscall 读错误不触发破坏性恢复。

结果、瓶颈、目标达成情况和未解决问题见 [性能报告](performance-report.md)；正确性证据及未覆盖场景见 [测试报告](test-report.md)。未改变单 Header/无 Footer Marker 的固有限制，没有增加其他存储架构层。
