# exec 性能调查

2026-10-18 在 S24 上的调查把 fork、状态传递、内部重新 exec 与 guest
加载拆开分析。测量结果以及被否决的紧凑状态实验见
[perf/results.md](../../../tawcroot/perf/results.md)；
命令和探针的局限见
[perf/README.md](../../../tawcroot/perf/README.md#exec-cost-investigation)。
后续的[执行技术调研](execution-techniques.md)
比较了 ARM64 重写、启动融合、fork server 以及 exec 重新设计。

隔离的分阶段剖析显示，内部 exec 之前约 223 µs，新运行时的 `main` 到
guest 入口之间约 189 µs，其中 supervisor 重建占 106 µs。exec 标记到
`main` 之间的 609 µs 既包括内核工作，也包括早期用户态启动。剖析会改变
状态布局并增加时钟调用；这些数值用于定位工作所在，而不是作为生产基线。

把 exec-state 头从 22,412 字节缩减到 1,940 字节，每次 spawn 约减少五次
minor fault，但受控 shell 中位数仅改善 0.84%，且顺序效应明显。该方案被
否决，生产环境仍使用 wire version 10。仅凭系统调用或 fault 次数降低，
不足以证明值得采用更复杂的状态交接。

## 归因约束

- A/B 对比时保持编译器、运行时、rootfs、CPU 亲和性和执行域一致。
  独立运行 `build.sh` 可能选中比 app 更新的 NDK；应显式设置
  `ANDROID_NDK_HOME`。在一个 dev APK 中只重新打包运行时，可以让 app
  其余部分保持不变。
- minor fault 涵盖 demand-zero、缓存文件和 COW 路径。Linux 的
  [file fault-around 实现](https://github.com/torvalds/linux/blob/v6.1/mm/memory.c#L4176)
  可能映射相邻页，因此访问到的页数和 fault 次数并不相同。
  应分别测量实际 fault 增量和访问路径耗时。
- [`/proc/stat`](https://docs.kernel.org/6.1/filesystems/proc.html#miscellaneous-kernel-statistics-in-proc-stat)
  报告的是系统/CPU 总量。它的变化无法确定单个子进程的内核时间。
  不要重复之前未经证实的、把约 500 µs 归因于进程专属内核工作的结论。
- 决定是否采用时比较未插桩的运行时，交替运行顺序，并通过 app 的
  exec broker 确认微基准结果。

## Loader 源码研究

当前的 mapper 即使在 `p_memsz == p_filesz` 时也会清零段的最后一个
不完整文件页；于是只读页需要临时的写权限。
[ELF ABI](https://refspecs.linuxfoundation.org/elf/gabi4%2B/ch5.pheader.html)
要求对 `p_memsz > p_filesz` 多出的区域做零填充。
[glibc](https://github.com/bminor/glibc/blob/master/elf/dl-map-segments.h#L136)
仅在存在该额外区域时才清零尾部。Linux 6.1 的
[主 ELF loader](https://github.com/torvalds/linux/blob/v6.1/fs/binfmt_elf.c#L1169)
与解释器路径的处理不同，因此不能据此断言所有原生 loader 对尾页的处理
都一致。本实验未包含任何 loader 改动；其测得的映射阶段仅约 14.5 µs。
