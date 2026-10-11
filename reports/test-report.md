# RAF 测试报告

测试采用先写核心用例、观察失败、再实现的流程。最初的 38 项用例在缺失行为的 stub 上全部失败，原始输出见 [tdd-red.log](raw/tdd-red.log)；核心实现完成后全部通过。独立审查的 14 种恢复重试实验后来并入一个核心用例，持续测量中发现的后台同步问题增加了两个先失败后修复的用例，最终共有 41 项核心测试。

| 类型 | 已覆盖场景 |
|---|---|
| 文件格式 | 独立 CRC 参考算法生成的 fixture；逐字节比较 Writer 输出；小端字段；空 Footer；零长度、二进制、变长和 3MiB payload |
| API | 单条及批量追加；批量超过 IOV_MAX；连续 ID；随机读取；零拷贝地址；迭代器结束；关闭后的调用；重复关闭；非法路径和同步间隔 |
| 并发 | 8 线程调用同一个 Writer；8 个不同文件 Writer；8 线程共享 Reader 并使用独立 Iterator；多个独立 Reader；线程和进程间 flock 冲突；Busy 不修改文件 |
| 校验 | 已知 CRC32C 向量；非对齐输入；流式 CRC；便携和运行时选择实现；Header/Footer CRC；恶意 Offset/边界；Payload 和 Length 损坏；关闭读 CRC 后完整扫描仍校验 |
| 恢复 | 未 Seal；进程直接退出；可识别版本的损坏 Header；不可识别 Header 不修改文件；空文件；中间损坏只保留前缀；8MiB 流式 payload；重复 Recovery |
| 任意尾截断 | 对含 4 条不同长度 Record 的 106B archive，在 0 至 106 的全部 107 个截断位置验证最长有效前缀；不足 16B 时必须拒绝恢复且不修改 |
| 系统调用注入 | writev EINTR、跨 iovec short write、零写入、ENOSPC；pread EINTR、short read、EIO；fdatasync EIO；ftruncate EIO；Footer 写入、Footer 同步、Header 写入及 Header 同步失败；撕裂 Header |
| 同步语义 | 无脏 Record 不调用同步；背景同步只处理脏数据；同步失败传播且粘滞；fdatasync 阻塞期间 Append 能完成；同步水位使用 syscall 开始前的 Record Count |
| 同步进度与在途错误 | 阻塞后续 writev 时，后台仍可同步先前完成的 Record；同步失败到达正在进行的 Append，已进入 syscall 的完整 Record 可以交由 Recovery 保留 |
| Recovery 中断后重试 | 未 Seal/已 Seal 两类文件，各自注入 Footer 写入、Footer CRC 写入、部分 Footer、第一次同步、Header 写入、部分 Header、第二次同步故障，共 14 种组合；重试后完整校验保留前缀 |

故障注入在测试可执行文件中通过链接器 `--wrap` 替换 syscall，生产库没有测试钩子或 I/O 抽象层。大部分操作仍在真实临时文件上执行，仅指定 syscall 被返回错误、缩短写入或阻塞。

读性能基准另有一项回归检查：构造只有最后一个字节非零的 8/9/79B payload，在 CRC 开/关时检查全部 payload 被消费，并核对写入字节数、文件实际大小、Record 数和同步计量。修正前明确失败于 `payload tail is never consumed`，见 [benchmark-red.log](raw/benchmark-red.log)；修正后通过。

独立审查检查了实现、规格和基准，当时未发现 Critical/Important 核心正确性缺陷。审查者重跑 13 项重点用例，并另行执行上述 14 种恢复重试实验。其提出的基准消费字节问题已通过回归测试修正；冷读共享缓存的解释已写入性能报告。随后的持续测量发现大 Batch 下后台等待追加锁而没有实际进行周期同步；新增用例先复现失败，再改为原子发布完成数、使用原有同步锁等待和同步，保证后台进度。另一个用例先复现后台错误未到达在途 Append，再增加粘滞错误检查。失败证据见 [background-progress-red.log](raw/background-progress-red.log)、[inflight-error-red.log](raw/inflight-error-red.log)。

最终 Release 与强制便携 CRC 构建：41 项核心用例和 benchmark_smoke 均通过（CTest 显示 2/2 测试程序），分别耗时 1.55s / 1.68s，见 [release-final.log](raw/release-final.log)、[portable-final-tests.log](raw/portable-final-tests.log)。最终 ASan/UBSan 构建同样通过，耗时 2.44s，见 [asan-final.log](raw/asan-final.log)。初始未优化的 ASan/UBSan 全套也通过，耗时 92.93s。TSan 本机启动失败：`unexpected memory mapping`。独立的空 main 程序也复现相同错误，见 [tsan-runtime-probe.log](raw/tsan-runtime-probe.log)，因此不能声称完成动态数据竞争检查。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
ctest --test-dir build --output-on-failure
cmake -S . -B build-portable -DCMAKE_BUILD_TYPE=Release -DRAF_PORTABLE_CRC=ON
cmake --build build-portable -j 8
ctest --test-dir build-portable --output-on-failure
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRAF_SANITIZE=ON
cmake --build build-asan -j 8
ctest --test-dir build-asan --output-on-failure
```

没有覆盖的实验：真实断电、介质/控制器错误、非协作程序直接修改/截断 mmap 文件、x86 和大端机器上的实际运行、接近 UINT32_MAX 的真实 payload 写入以及超大索引导致的内存不足。已验证进程退出和 syscall 故障；这些实验不能替代硬件断电测试。文件格式本身的恢复限制继续遵循 design.md。
