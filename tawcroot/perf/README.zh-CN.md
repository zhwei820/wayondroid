# tawcroot 性能测试工具

[English](README.md) · 简体中文

独立的微基准测试，用于比较：

- 在宿主上以 rootfs 路径前缀 `native` 直接执行
- `tawcroot -r ROOTFS -- CMD`
- `proot -r ROOTFS CMD`
- 真正的 `chroot ROOTFS CMD`
- gVisor `runsc`，以 rootfs 作为 OCI bundle 根目录

它不属于 `tawcroot/test.sh`，也不属于集成测试套件，仅用于本地性能测试。

历史结果记录在 [results.md](results.md)；新的测试结果要与这些基线对比，有意义的结果追加到该文件。

## 快速开始

如有需要，先构建宿主版 tawcroot：

```sh
tawcroot/build.sh --abi=host
```

运行所有可用后端：

```sh
tawcroot/perf/run-perf.sh --rootfs /path/to/rootfs
```

运行一次针对性对比：

```sh
tawcroot/perf/run-perf.sh \
  --rootfs /path/to/rootfs \
  --modes native,tawcroot,proot \
  --iterations 50000 \
  --rounds 5
```

运行器把 TSV 写入 `build/tawcroot-perf/results-*.tsv`，并打印 mean/median/min/max `ns_per_op` 汇总行。

## 在设备上

`run-perf-device.sh` 在 arm64 手机（`TAWC_TARGET` / `ANDROID_SERIAL`）上对比 native 与 tawcroot：从 adb `shell` 域运行，使用 scratch 目录中的最小 rootfs，各轮交错执行：

```sh
tawcroot/build.sh --abi=aarch64
TAWC_TARGET=physical tawcroot/perf/run-perf-device.sh \
  --rounds 10 --iterations 20000 --cpu 200
```

`--cpu` 通过 `taskset` 绑核（十六进制掩码；选超大核 —— 不绑核时核间迁移会让结果波动 2 倍）。shell 域没有 app 的 seccomp 过滤器，也没有 `untrusted_app` SELinux 策略，因此 Android 对 app 拦截的系统调用（例如 16+ 上的 `openat2`）在这里仍然可用；凡是对策略敏感的结论都要在 app 中确认。基准测试需要一个支持静态链接的 `aarch64-linux-gnu-gcc`（或使用 `--cc` / `--bench-bin`）。

### exec 开销排查

在把进程启动开销归因之前，先把 fork 与 exec 分开：

```sh
TAWC_TARGET=physical tawcroot/perf/run-exec-cost.sh --rounds 5 --cpu 9
```

它在 shell 域中对比 native 与 tawcroot，使用同一个静态 glibc 探针，以及一个不做 libc 启动的 guest。它记录墙钟时间、父/子进程 CPU 时间，以及实际的 minor/major 缺页增量。内核探针覆盖映射、memfd 状态传递和缺页。`--cpu` 接受 CPU 列表；运行器会把它转换成 Android 的十六进制掩码。原始 TSV 和运行时哈希保存在 `build/tawcroot-perf/` 下。使用 `--tawcroot BASELINE --compare CANDIDATE` 做交错对比。app 域的性能仍需单独做 broker/真实负载 A/B。两个运行时要用同一个 `ANDROID_NDK_HOME` 构建；与 APK 对比时使用该 app 锁定的 NDK。

对于 exec 内部各阶段，`python3 tawcroot/perf/build-exec-profile.py` 使用 `build.sh` 中的 NDK 参数，在 `build/tawcroot-perf/` 下构建一份隔离的插桩副本。它不会放入 APK。直接从 shell 启动生成的运行时；它固定的系统调用 stub 地址属于该二进制。每次成功加载会在进入 guest 之前打印一行 `TAWC_EXEC_PROFILE`。`kind=child` 行包含跨 re-exec 的前/后绝对单调时间戳。输出格式化发生在 `post.jump` 之后；归因使用阶段间差值，性能对比使用普通二进制。

profiler 扩展了复制过去的 exec-state 头，并通过共享映射传递标记。它的开销使得插桩版的 spawn 时间不能当作生产基线。minor 缺页还包括按需清零、已缓存文件缺页和 COW；文件 fault-around 意味着被访问的页数与缺页次数不一致。CPU 全局的 `/proc/stat` 变化无法确定某个特定进程的内核时间。

要在不重新构建无关 APK 组件的情况下做 app A/B，使用 `repack-runtime-apk.sh DEV_APK ARM64_RUNTIME OUTPUT_APK`。它只替换一个运行时条目，对 APK 做对齐/签名，并校验原有的 debug 证书。两次构建要使用同一个输入 APK。`macro.sh --cpu 9
--workloads spawn_true_x100,bash_startup_x50,stat_many` 把真实 app 对比收窄到进程启动和一个文件系统对照项。负载失败或缺少计时行时，macro 运行会在写入结果文件之前失败。

### ARM64 改写可行性

```sh
TAWC_TARGET=physical tawcroot/perf/run-rewrite-probe.sh --cpu 9
```

它构建一个静态探针，并在原生 shell 和通过 app broker 进入的已安装发行版中运行同一个二进制。它记录 app 的 SELinux 上下文和已安装运行时的哈希，并清理两份 scratch 副本。如有需要，用 `TAWC_INSTALL_ID` 选择安装实例。

探针把一条受控的 `getuid` SVC 打补丁为跳转到附近一个固定返回值的 trampoline。它检查 RX/RWX 切换、指令缓存刷新、GPR/SP/NZCV 保持、打补丁后由四个线程执行，以及恢复原始指令。打补丁发生在创建线程之前；并发的补丁发布和 FP/SIMD 上下文不在本探针范围内。退出码 77 表示不可用，1 表示检查失败，0 表示成功。

`patched_fixed_return` 测量的是一段很小的分支序列，而不是真实的 TAWC 分发、参数校验或路径转换。其耗时不能作为应用加速比来报告。它使用与 exec 开销探针相同的可选静态 aarch64 glibc 工具链；不会启用任何生产依赖或运行时打补丁。

## 不经过 app 的真实负载

`macro.sh` 和 `trap-stats.py` 走 app 的 exec broker，会在手机上启动 app 的 activity（而且替换 APK 会让它每轮都重新启动）。要在不碰 app、不碰屏幕的情况下做运行时对运行时的 A/B，`run-macro-shell.sh` 会把一个发行版 tarball 一次性解压到设备 scratch 目录，然后从 adb `shell` 域运行两个运行时：

```sh
TAWC_TARGET=physical tawcroot/perf/run-macro-shell.sh \
  --rootfs-tar build/tawcroot-perf/ArchLinuxARM-aarch64-latest.tar.gz \
  --tawcroot BASE.so --compare CANDIDATE.so --rounds 3 --cpu 9
tawcroot/perf/run-macro-shell.sh --cleanup   # remove the rootfs
```

rootfs 在多次运行之间保留在设备上（只有第一次需要传 `--rootfs-tar`）。

对于小的用户态改动，`run-insn-ab.sh BASE.so CAND.so` 在同一组负载上对比 `simpleperf stat -e instructions:u`。即使繁忙或处于 doze 的手机让墙钟时间波动 2-3×，指令数的重复误差也在 ~0.01% 以内，但它只覆盖 guest + tawcroot 的用户态：系统调用次数和内核路径的变化仍需看墙钟时间。shell 域缺少 app 的 SELinux/seccomp 上下文，所以用它来比较运行时之间的差异，而不是 app 的绝对数值。

## 真实负载的时间花在哪里

`trap-stats.py` 把 app 域负载的时间归因到 tawcroot。用 `EXTRA_CFLAGS=-DTAWCROOT_STATS tawcroot/build.sh
--abi=aarch64` 构建一个测量用运行时（生产构建会把这些钩子编译掉；见 `include/stats.h`），用 `repack-runtime-apk.sh` 换入，然后：

```sh
TAWC_TARGET=physical tawcroot/perf/trap-stats.py --cpu 9 [--workloads a,b]
```

对每个负载，它打印墙钟时间、拦截次数及 handler 时间、tawcroot 自身每次拦截发出的系统调用数、路径缓存命中率、`/proc/self/fd` readlink 的来源，以及按阶段拆分并附 minor 缺页数的 `--exec-child` 初始化。handler 时间不含 SIGSYS 投递和返回。测完后重新安装生产运行时。

不经过 app：在 `run-macro-shell.sh` 解压好 rootfs 之后，`trap-stats.py --shell <stats-built libtawcroot.so>` 从 adb shell 域运行同样的负载（不安装、不启动任何东西；二进制和统计文件事后删除）。stats 构建会关闭 `TAWCROOT_FASTPATH`：被改写的调用点会绕过 SIGSYS handler，从而不被计数。

## 本地竞品工具

这些工具仅用于本地性能测试。不要把它们加入正常的 app 构建。

在 x86_64 宿主上，把本地的 `proot` 和 `runsc` 二进制安装到 `build/tawcroot-perf/tools/` 下：

```sh
mkdir -p build/tawcroot-perf/tools
cd build/tawcroot-perf/tools

gvisor_base="https://storage.googleapis.com/gvisor/releases/release/latest/$(uname -m)"
curl -fsSLO "$gvisor_base/runsc"
curl -fsSLO "$gvisor_base/runsc.sha512"
sha512sum -c runsc.sha512
chmod 0755 runsc

curl -fL -o proot https://proot.gitlab.io/proot/bin/proot
chmod 0755 proot
```

然后显式传入：

```sh
tawcroot/perf/run-perf.sh \
  --rootfs /path/to/rootfs \
  --modes native,tawcroot,proot,gvisor \
  --proot build/tawcroot-perf/tools/proot \
  --runsc build/tawcroot-perf/tools/runsc \
  --iterations 50000 \
  --rounds 20 \
  --interleave
```

较旧的 GitHub `proot-static-build` v5.1.1 二进制在测试时于当前开发宿主上发生段错误。除非你确实想与那个旧版本对比，否则使用当前上游的 PRoot 二进制。

## 依赖

- 与本机架构匹配的 Linux rootfs。
- 一个静态 C 编译器路径，通常是 `musl-gcc`，或可用静态 libc 的宿主 `cc`。也可以传 `--bench-bin PATH`。
- `tawcroot` 后端需要 `build/tawcroot-host/tawcroot`，或传 `--tawcroot PATH`。
- 可选：`proot` 后端需要 `proot`。
- 可选：`gvisor` 后端需要 `runsc`。
- 可选：`chroot` 后端需要 root，或使用 `--sudo-chroot`。

对于无 root 的 gVisor，rootfs 路径必须能被当前用户遍历。一个一次性的测试 rootfs 通常至少需要：

```sh
chmod 0755 /path/to/rootfs
chmod 1777 /path/to/rootfs/tmp
```

基准测试二进制会被复制到：

```text
<rootfs>/tmp/tawcroot-perf/tawcroot-perf
```

负载会在以下位置创建文件：

```text
<rootfs>/tmp/tawcroot-perf/work
<rootfs>/tmp/tawcroot-perf/create
```

## 负载

- `getpid`：基线系统调用循环。
- `stat`：重复的路径元数据查询。
- `open_read_close`：重复的路径 open/read/close。
- `readdir_entries`：重复的目录扫描。
- `create_write_unlink`：创建/写入/删除循环。
- `fork_exec_wait`：小型 fork/exec/wait 循环。
- `gettid`：不被拦截的系统调用（只有过滤器开销）。
- `getuid`：被拦截、无路径处理（SIGSYS 往返的下限）。
- `fstat`、`write_read_pipe`：热点的纯 fd 系统调用。
- `stat_deep`、`stat_leaf_symlink`、`stat_relative`、`fstatat_dirfd`：给解析器施压的路径形态（8 级路径分量、相对路径的末端符号链接、相对 cwd、相对 dirfd）。
- `signal_raise`：tgkill + 投递 + 被拦截的 `rt_sigreturn`。

这些是微基准测试，不是应用基准测试。用它们发现较大的系统调用路径回归，然后再用真实负载验证，例如包管理器操作或浏览器启动。

## 解读结果

比较后端时优先看 `median` 而不是 `mean`。打印 `min`/`max` 是为了让调度器和 CPU 频率噪声可见。

要得到值得讨论的数字，至少使用 `--rounds 20`。如果结果看起来出乎意料，换一个后端顺序重跑，例如：

```sh
tawcroot/perf/run-perf.sh \
  --rootfs /path/to/rootfs \
  --modes proot,tawcroot,native,gvisor \
  --proot build/tawcroot-perf/tools/proot \
  --runsc build/tawcroot-perf/tools/runsc \
  --iterations 50000 \
  --rounds 20
```

把 `getpid` 当作健全性检查，而不是 rootfs 基准测试。有些后端可能对它走快速路径，而不经过路径转换。

除非传入 `--interleave`，运行器总是先把一个后端的所有轮次跑完，再切到下一个后端。按后端顺序执行简单且可复现，但可能保留缓存和 CPU 频率的顺序效应。要做严谨对比，使用 `--interleave`、比较至少两种后端顺序，或检查原始 TSV。
