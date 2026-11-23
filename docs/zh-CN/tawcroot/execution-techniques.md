# 其他执行技术

2026-10-18 在紧凑 exec-state 实验之后所做的源码调研。本文记录可行性与假设，
不是实测的改进，也不是实现计划。当前耗时数据见
[exec-performance.md](exec-performance.md) 和
[perf/results.md](../../../tawcroot/perf/results.md)。

## 按目标划分的候选方案

| 技术 | 可避免的工作 | 范围 / 评估 |
| --- | --- | --- |
| ARM64 系统调用改写，以 SIGSYS 兜底 | 已打补丁的系统调用点上的信号投递与返回 | 最值得关注的新拦截技术；影响 guest 反复发出的系统调用，不只是 exec |
| 初次启动时传入干净的 guest 环境 | 加载 `/usr/bin/env` 及其随后的内部 re-exec | 相对有界的顶层启动改动；对已存在 shell 的 fork/exec 循环无影响 |
| 单线程原生 fork server | 新应用启动时反复的运行时/supervisor 初始化 | 较大的生命周期/IPC 改动；无法在现有 guest PID 内透明替代 exec |
| 每次 exec 共用一块 arena | 反复的 argv/env 长度扫描、复制、序列化和重映射 | 已构建并测量：S24 上 spawn 中位数 −1.1%，已否决（results.md） |
| 进程内 guest exec | 内核 re-exec 与运行时重建 | 潜在范围很大，但 Linux exec 语义和共享 VM 的子进程使通用化很困难 |

## ARM64 改写

[svc-hook](https://github.com/retrage/svc-hook) 把 ARM64 `svc` 指令替换为等宽的
`b`，跳到附近（约 128 MiB 以内）的 trampoline。其声明的目标包括 Android。其
[implementation](https://github.com/retrage/svc-hook/blob/main/main.c)
会修改代码页保护、保存上下文、处理 clone/信号等特殊情况并刷新指令缓存。它是有用
的参考材料，而不是已经与 tawcroot 兼容的库。

[gVisor's systrap description](https://gvisor.dev/blog/2023/04/28/systrap-release/)
同样展示了把频繁被拦截的系统调用点替换为直接的用户态跳转；该文中的打补丁实现是
x86 专用的。[ASC-Hook](https://arxiv.org/html/2412.05784v2) 是另一种 ARM 设计，
但其局限包括 trampoline 放在低地址、延迟加载的库以及与信号 handler 冲突。已发表
的 hook 微基准不能用来预测 tawcroot 工作负载的加速。

一种 tawcroot 特有的混合方案可以对未打补丁的点保留现有 seccomp/SIGSYS 路径，
只对经过验证的热点打补丁。这样就不要求立即改写每个 DSO 或 JIT 页。当前的同进程
分发器和固定的原始系统调用 stub 仍然有用。初期宜将直接分发限定于普通的路径/fd
操作；clone、exec 和信号返回需要各自的上下文/栈处理。不要把本来就不被拦截的调用
替换成更慢的通用分发。

[controlled S24 probe](../../../tawcroot/perf/results.md#2026-10-18-arm64-rewriting-feasibility-s24)
在 app 域中通过了近距离 trampoline 分配、代码保护状态切换、GPR/SP/NZCV 保持、
打补丁后的四线程执行以及原始代码恢复。它返回一个常量而不是调用真正的分发器，因此
证明的是可行性，而不是应用层加速。FP/SIMD 保持、guest 栈空间、并发发布、
unmap/remap 以及库的延迟加载仍需显式处理。Android 的执行策略必须以 app 身份检查，
不能只在 adb shell 下检查。
当前的 [mapper](../../../tawcroot/src/loader_map.c) 在临时写入 ELF 尾页时已经
保留执行权限。
[Android 6.1 SELinux hook](https://android.googlesource.com/kernel/common/+/refs/heads/android14-6.1/security/selinux/hooks.c)
会区别对待给已修改的私有映射增加执行权限的情况；不能简单假定通用的 RW 到 RX
做法可行。

## 启动方式改动

[TawcrootMethod.rootfsArgv](../../../app/src/main/java/me/phie/tawc/install/TawcrootMethod.kt)
在前面加上 `/usr/bin/env -i K=V...`；
[main.c](../../../tawcroot/src/main.c) 中的 `prod_main` 把它作为第一个 guest 加载。
[GNU env](https://github.com/coreutils/coreutils/blob/master/src/env.c)
随后 exec 所请求的命令。一个直接提供最终 guest 环境并直接加载该命令的启动器，
可以在每次顶层启动时省掉一整次 exec。这是从源码推导出的机会；其耗时收益尚未测量。

如果要让 `/proc/<pid>/environ` 保持真实，初始的内核 exec 就必须已经收到正确的
guest envp。只改合成的 ELF 栈是不够的。ProcessBuilder、PTY JNI 和远程启动都需要
一致的环境替换，包括 TERM/COLORTERM、PATH、cwd，以及与宿主 TMPDIR 的隔离。当前
的 PTY 依赖用 `putenv` 添加环境条目，而不是清除继承来的条目，因此这不只是删掉一个
argv 前缀那么简单。

[Android-style zygote](https://source.android.com/docs/core/runtime/zygote)
或 [Chromium-style fork server](https://chromium.googlesource.com/chromium/src/+/main/docs/linux/zygote.md)
则会保持一个单独启动的、单线程的原生运行时随时准备 fork。它必须负责 stdio/PTY
fd 传递、会话、子进程退出报告以及配置/发行版的生命周期。fork Android JVM 进程
不是所提议的设计。由 server 创建的子进程无法在不改变 PID 和父子关系的前提下顶替
现有 guest 的 exec。没有内核 exec，其内核 cmdline/environ 也仍然是 server 的；
每个请求的元数据需要另行解决。当前的 proctitle 改写受限于初始的内核参数区。

## 保留与不保留内核 re-exec 的 guest exec

保留内核 re-exec 时，每次 exec 的 arena 可以从收集到 commit 全程直接作为最终的
共享 memfd 映射。当前 [syscalls_exec.c](../../../tawcroot/src/syscalls_exec.c)
收集字符串，[exec_handler.c](../../../tawcroot/src/exec_handler.c) 序列化它们，
[exec_state.c](../../../tawcroot/src/exec_state.c) 再重复做长度检查。在插桩运行
中，收集/序列化/commit 分别约占 49/51/25 µs，其中只有一部分是冗余的。批量的安全
guest 读取和 exec 失败后的清理必须保留；每个并发/共享 VM 的 exec 都需要独立的所有
权。仅仅把复制改成共享映射，针对的是已测得的约 6.3 µs 的小差异。

不可变的 supervisor 描述符可以复用规范化后的 root/bind 配置，但不能盲目缓存可变
的符号链接、cwd、身份或 shm。guest 的 chroot/mount 以及外部写入需要相应的版本化
或重新校验。复用 root/bind/store fd 等于重复已被否决的 fd 继承实验。106 µs 的
supervisor 阶段并不能全部去掉。

去掉内核 re-exec 是一个本质上不同的项目。在
[glibc posix_spawn](https://github.com/bminor/glibc/blob/master/sysdeps/unix/sysv/linux/spawni.c)
中，子进程用 CLONE_VM|CLONE_VFORK 与父进程共享 VM。Linux 的
[exec release path](https://github.com/torvalds/linux/blob/v6.1/kernel/fork.c)
会唤醒等待中的父进程。而 unmap guest 内存再跳转到另一个 ELF，则会改动父进程的
VM，并破坏这一释放协议。当前 tawcroot 直接放行 CLONE_VM，对所有权的跟踪也不足以
挑出一个安全的特例。内核 exec 还要处理其他线程、CLOEXEC/共享 fd 表、信号处置/
altstack、TLS、robust futex、brk 以及进程元数据；见
[execve contract](https://man7.org/linux/man-pages/man2/execve.2.html)。
一个普通 fork、私有 VM、单线程的子集，在能使用单独的快速路径之前需要先得到证明。

## 优先级较低或不可用的替代方案

- **SUD：** Linux 6.1 ARM64 未实现 Syscall User Dispatch；仅有内核版本 5.11+
  并不够。[current manual](https://man7.org/linux/man-pages/man2/PR_SET_SYSCALL_USER_DISPATCH.2const.html)
  指明支持 x86，并注明设置在 fork/clone/exec 后不保留。SUD 也仍然会投递 SIGSYS。
- **seccomp 用户通知：**
  [Linux 6.1 interface](https://www.kernel.org/doc/html/v6.1/userspace-api/seccomp_filter.html)
  增加了监听器和请求/应答操作；它无法放宽继承来的 Android 过滤器。没有证据表明
  用这种 RPC 替换 tawcroot 的本地 handler 能改善其 exec 工作负载。
- **gVisor 共享队列/自旋：** 解决的是 Sentry 与沙箱任务之间的通信。tawcroot
  已经在发出调用的线程内处理调用，没有那种 IPC。单独加一个队列不会消除任何现有
  开销。
- **io_uring/eBPF：** Android 17 的
  [app-domain policy](https://android.googlesource.com/platform/system/sepolicy/+/android17-release/public/te_macros)
  阻止 io_uring，
  [BPF policy](https://android.googlesource.com/platform/system/sepolicy/+/android17-release/private/bpfloader.te)
  把程序加载限制给 bpfloader。两者都不是可直接替换的、无 root 的路径转换 exec
  机制。
- **ELF 计划缓存/持久化池：** exec 前目前只探测 ELF 头，而不是完整的可复用加载
  计划。exec 后的 ELF 解析实测只有 5.3 µs；身份、权限以及可变的文件内容仍需校验。
  可复用 guest VM 槽位的池在不做结构性改动的情况下无法跨越内核 exec 存活。

有用的划分是：拦截开销、guest exec 开销、顶层应用启动，三者分开看。一项技术在
其中一项上的成功，并不能证明在其他项上也有改进。改写探针与生产运行时隔离，不增加
任何运行时依赖。
