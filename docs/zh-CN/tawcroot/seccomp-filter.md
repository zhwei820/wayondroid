# tawcroot — seccomp 过滤器

## Seccomp 过滤器

手写 cBPF。被拦截系统调用的列表就是运行时
分发表 —— `prod_rootfs_init` 通过
`tawcroot_dispatch_trap_list` 遍历它，收集所有槽位
非 NULL 的系统调用号，`filter.c` 再据此列表生成 BPF 程序
并安装。分发表是 handler 路由与 allowlist 共用的
唯一事实来源。程序结构：

```
load arch from seccomp_data (offset 4)
if arch != AUDIT_ARCH_<our-arch>: KILL_PROCESS  // can't happen, defense in depth
load syscall_nr from seccomp_data (offset 0)
binary search over the sorted trap set (+ rt_sigreturn):
  not in set              → ALLOW
  close, args[0] < 1000   → ALLOW   // closefrom fast path
  fcntl, args[0] < 1000, args[1] not F_ADD_SEALS/F_GET_SEALS
                          → ALLOW   // only reserved fds and shm seals need us
  clone with CLONE_VM     → ALLOW   // threads, vfork
  otherwise               → JA ipcheck
ipcheck:  // cBPF is 32-bit — compare instruction_pointer in two halves
  load instruction_pointer[31:0]  (offset 8)
  load instruction_pointer[63:32] (offset 12)
  ip == &tawcroot_raw_syscall_ret → ALLOW   // our stub
  else                            → TRAP
```

**先判 NR，只对拦截候选才判 IP。** 未拦截的系统调用
仅凭 arch + nr 即可判定，因此内核的 seccomp action cache
（5.11+）会把它标记为恒定 ALLOW，完全跳过过滤器；较老的
内核也只需走约 8 次比较（`LEAF_RUN` 个 JEQ 叶子挂在一棵
JGE 树下，由 32 位 `JA` 连接，所以子树大小从不会限制跳转）。
`filter_build.c` 自行对拦截列表排序并去重。
`test_filter.c::filter_untrapped_nr_reads_only_arch_and_nr` 固定了这种
可缓存的形态。

**实现注意：** seccomp cBPF 以 32 位字为单位操作。
`seccomp_data.instruction_pointer` 是位于 offset 8 的 `__u64`（在
offset 0 的 `int nr` 和 offset 4 的 `__u32 arch` 之后）；必须对两半
（offset 8 和 12）分别 `BPF_LD_ABS_W` 并各自比较。在 aarch64 上基址为 0x2000000000 时，高 32 位
非零 —— 只检查低半部分的过滤器会
在低地址恰好相同的 guest 代码上错误地 ALLOW。

**惰性拦截。** 通过
`tawcroot_dispatch_install_lazy` 注册的 handler 不进入基础过滤器：`write`
（uid_map 模拟）、`getpid`/`getppid`/`kill`/`tgkill`（PID
namespace 模拟）只有在 guest 创建了伪
namespace 之后才有事可做。此时 `namespace.c` 再叠加一个只拦截
这些调用的小型第二过滤器（`tawcroot_install_lazy_filter`，使用 `SECCOMP_FILTER_FLAG_TSYNC`，
因为模拟的 `unshare` 没有单线程规则）。叠加的
过滤器只会增加 TRAP，会跨 fork 以及
`--exec-child` 重新 exec 继承，而蕴含这些过滤器的 namespace 状态
也以同样方式继承，所以“已激活”即意味着“已被拦截”。
由 `test_prod_features.c::prod_lazy_namespace_traps` 覆盖。

`tawcroot --init` 在启动时叠加同一个 PID 过滤器，因此第一个
guest 进程是 pid 1，且没有内核 namespace（这是 rootless 运行
systemd 的基础）。它同时是 `PR_SET_CHILD_SUBREAPER`，所以孤儿进程
会重新挂到它名下，其 `getppid()` 映射为 1。以 init 为目标的 `kill`/`tgkill`
会丢弃其 `/proc/<pid>/status` SigCgt 中不存在的信号，
包括 SIGKILL，与 namespace 中的 init 一致；来自 guest 外部的 SIGKILL
仍然生效。由 `prod_init_session` 覆盖。
在 init 会话中，PID 过滤器还会拦截 `tkill`、
`rt_(tg)sigqueueinfo` 和 `pidfd_open`：只有 init 及其
后代（按 `/proc` ppid 链）才是 guest。`kill(-1)`/`kill(0)`/
`kill(-pgrp)` 枚举 `/proc` 并只向 guest 发信号，发给其他 pid 的
单目标信号得到 `ESRCH`，并且 `/proc` 上的 `getdents64` 会隐藏
它们。由 `prod_init_pid_isolation` 覆盖。
init 会话为 systemd 模拟的其他内容见 [systemd.md](systemd.md)。

有两个过滤器同时生效：Android 现有的 `untrusted_app` 过滤器
（在我们获得控制权之前已由 zygote 加载），加上我们的过滤器
（叠加在其上）。内核在每次系统调用时求值所有过滤器；
优先级最高的结果胜出。优先级是
内核定义的 seccomp action 顺序，不是我们在
tawcroot 里自创的规则：

```
KILL_PROCESS (0x80000000) > KILL_THREAD (0x00000000) > TRAP (0x00030000) >
ERRNO (0x00050000) > USER_NOTIF (0x7fc00000) > TRACE (0x7ff00000) >
LOG (0x7ffc0000) > ALLOW (0x7fff0000)
```

不要把它重新实现成任意的数值排序。这些常量的
编码使上述文档化的顺序在当前可行，但维护
契约是“遵循 Linux 的 action 优先级”；使用这些常量，
并以下面的行为测试为事实来源。

与 Android 过滤器的关键交互：
- Android 返回 ERRNO，我们返回 TRAP → **TRAP 胜出**。这正是
  `notes/proot.md` 中记录的 x86_64 上 lp64-`access` 情形
  所需要的。
- 两个过滤器对同一系统调用都返回 TRAP → **只有一次 SIGSYS**，
  `RET_DATA` 取自最近安装的过滤器（我们的）。不会
  重复投递。
- 我们返回 ALLOW，Android 返回 TRAP → **TRAP 胜出**。我们无法
  撤销先前过滤器的任何 TRAP。这就是
  `--exec-child` 不能安装新过滤器的原因 —— 见 §"为什么用
  non-PIE"。**这也意味着我们可能收到并未要求拦截的
  系统调用的 TRAP** —— 对于 NULL 分发项，handler 必须返回 `-ENOSYS`，
  而不是 abort（见 §"SIGSYS handler"）。
  未来的 Android 版本可能 TRAP 更多系统调用。
- 如果 Android 某天对某个系统调用 KILL_PROCESS，我们就输了 —— 但这
  对 Android 上任何沙箱技术都成立，并非我们独有。

### 以非 root 身份安装过滤器

`PR_SET_NO_NEW_PRIVS` 是让非特权进程得以
安装 seccomp 过滤器的关键。在 `seccomp(SECCOMP_SET_MODE_FILTER, …)` 之前设置它。
不需要 `CAP_SYS_ADMIN`，没有 SELinux 限制 —— 已确认
在 Android 的 `untrusted_app` 域中可用。

注意：bionic **不**提供 `seccomp()` libc 封装。直接使用
`syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, flags, &prog)`。

过滤器只在 tawcroot 初次调用时安装**一次**。
它作为内核状态跨 `fork` 和 `execve` 继承。guest 的子进程
同样会被过滤，这正是我们想要的 —— 一个派生出 `pacman` 的 `bash`
仍然应该转换路径。
**`--exec-child` 不安装新过滤器** —— 继承来的
过滤器是正确的，因为 stub 地址是链接期常量
（non-PIE）。见 §"为什么用 non-PIE"。

相比之下，SIGSYS handler 是进程本地的（内存，而非
内核状态）。它跨 `fork` 被复制，但**跨 `execve` 被重置为
`SIG_DFL`** —— execve 之后的 guest 有过滤器
（内核状态，保留下来）却没有 handler。SIGSYS 的默认动作
是终止进程。这是我们必须解决的核心问题。
两种已知方案：

#### 方案 A：先重新 exec 到自身

让 guest 的 `execve` 经过我们。具体做法：把
每个 `execve("/bin/foo", argv, envp)` 改写为带一个状态 fd
重新调用我们的二进制：
`execveat(our_binary_fd, "", ["tawcroot", "--exec-child",
"--state-fd=NN"], envp, AT_EMPTY_PATH)`。状态 fd 中包含
原始目标路径、guest argv 字符串、当前 root 状态、rootfs
路径、bind 规格以及 tawcroot 标志。envp 是 *guest 的* envp，
原样透传 —— `--exec-child` 不得往环境里
插入任何自己的东西。我们的二进制检测到
`--exec-child` 后，读取并关闭状态 fd，重新安装 SIGSYS
handler（被 `execve` 重置），并重新打开 rootfs fd / bind 表。
它**不**安装新的 seccomp 过滤器 —— 继承来的过滤器
是正确的，因为 stub 位于相同地址（non-PIE，见
§"为什么用 non-PIE"）。随后它通过下文的
PT_INTERP 流程启动真正的目标。

**关键自举约束：** 从启动 `--exec-child` 的 `execveat`
到 handler + rootfs
fd 完全初始化之间，继承的 seccomp 过滤器已生效，但
没有 handler。SIGSYS 的默认动作是进程
终止。因此 **`--exec-child` 的整个初始化路径 ——
读状态 fd、安装 handler、打开 rootfs fd、建立 bind 表、
ELF 加载 —— 必须让自己发出的所有系统调用都经过
`tawcroot_raw_syscall()`**，继承的过滤器按 IP 对其
ALLOW。在跳转到 guest 之前，不得对被 TRAP 的系统调用使用 libc 封装（`openat`、`stat`、
`readlink`、带路径参数的 `mmap` 等）。
`sigaction` 是安全的（未被 TRAP）。静态 bionic 的
`memcpy`、`memset`、字符串函数是安全的（无系统调用）。这
与 handler 本身遵循的纪律相同 —— handler 使用
stub 来避免递归；`--exec-child` 初始化用它来
熬过无 handler 窗口和无 rootfs fd 窗口。

优点：概念模型清晰，每个 guest 进程都是一次全新的
tawcroot 初始化。

缺点：每次 guest exec 都变成一次宿主重新 exec 到 tawcroot，外加
对真实目标的一次手动 ELF 加载/跳转。单次 exec 的开销
单看很小，但在 fork 密集的负载上会累积（`pacman -Qkk`、
configure 脚本、Firefox 内容进程派生）。值得在
MVP 跑起来后测量。

这是我们起步采用的方案。本质上它就是 proot 的
loader 所做的事，只不过我们的“loader”和入口点是
同一个二进制 —— **但前提是 `libtawcroot.so` 是静态链接的。**
否则 bionic linker 会在继承的过滤器下、未安装 handler 的情况下
打开 libtawcroot 的 `DT_NEEDED` 库，
进程在 `--exec-child` 运行之前就会死掉。见 "Known gaps" #1。

#### 方案 B：跨 execve 保留的进程内 trampoline

用 `mmap(... MAP_FIXED ...)` 把我们的 handler + 全局变量放在一个
固定地址，然后借助类似 preload 的手段声称它被“拥有”。
实际上行不通 —— `execve` 会无条件清除映射。
跨 `execve` 存活的唯一方式是位于新程序的
text 中，或被重新加载。

结论：方案 A 加手动加载 guest。每次 guest `execve`
都变成一次 tawcroot 的重新 exec。`--exec-child` 进程
重新安装 handler，然后手动加载真实目标并跳转
过去，不再执行另一次 `execve`。seccomp 过滤器被继承，因此
系统调用持续被拦截；handler 保持有效，因为
最终转入 guest 代码是一次进程内跳转，而不是 exec。

这意味着**二进制必须能从 guest 的上下文中被 exec**。
用 Android 的术语，就是 `nativeLibraryDir`
（`/data/app/~~<hash>/me.phie.tawc-<hash>/lib/<abi>/libtawcroot.so`），
它具有 `apk_data_file` SELinux 上下文，与 proot
对 `libproot-loader.so` 使用的技巧相同。所以我们以 `libtawcroot.so` 发布，
路径保留在 `nativeLibraryDir` 中。

因此 `libtawcroot.so` 是一个可直接 `execve` 的 non-PIE 静态
二进制，只是*名字*像共享对象 —— 与 proot 的
`libproot-loader.so` 用的模式相同（同样是一个 ET_EXEC 静态二进制，改名
为 `.so` 以便 jniLib 提取）。Android 的 jniLib 提取器只
按 `lib*.so` 文件名匹配 —— 不校验 ELF 类型。
用 `-static -no-pie` 和显式 `_start` 构建；构建
脚本把它作为可执行文件链接，按架构使用
`-Wl,-Ttext-segment=<base-addr>`（地址选择见 §"为什么用 non-PIE"），
以及
`-Wl,-soname,libtawcroot.so`，让 jniLib 打包步骤
满意。APK **必须**在 manifest 中设置 `extractNativeLibs="true"`
（现状已是如此 —— proot 的 `libproot-loader.so` 有
同样的约束）。不提取的话，jniLibs 会直接从 APK
mmap，不会以内核可以 `execve` 的文件形式存在。

显式 `_start` 很重要。在 tawcroot 安装或重新安装 `SIGSYS`
handler 之前，不要依赖不透明的静态 bionic
启动代码，因为在 `--exec-child` 中继承的 seccomp 过滤器已经
生效。启动契约是：

- `_start` 在不依赖 libc 的情况下解析原始初始栈，足以找到 `argc`、`argv`、
  `envp` 和 auxv。
- 如果这是 `--exec-child`，在 handler 生效之前的所有系统调用都
  经过 `tawcroot_raw_syscall()`，且只经过这个 raw stub。
- 我们选择调用的任何 bionic/libc 初始化都必须审计其在
  继承过滤器下的系统调用。如果运行时路径跳过 bionic 初始化，
  要写明哪些 libc 设施因此在手动跳转到 guest 之前
  被禁止使用。
- handler/运行时代码不依赖 bionic TLS、`errno`、stdio、
  malloc、pthread 状态或 property-service 初始化。初始化代码
  只能在第一次安装过滤器之前，或在
  `--exec-child` handler 安装之后使用更高层的辅助函数。

副作用：二进制需要知道自己的路径（以便重新 exec
自身）。要么在编译期通过 `-D` 写死（脆弱 ——
APK 路径在重新安装时会变），要么在启动时读取 `/proc/self/exe`
并保存。**两个独立保存的路径，两种独立用途：**

- **`our_binary_path`** —— `libtawcroot.so` 在磁盘上的真实路径
  （在初次启动以及每次
  `--exec-child` 初始化时，经 raw stub 对 `/proc/self/exe` 调用 `readlinkat` 读取，时机在 handler 安装之前或
  guest exe 保存值设置之前 —— 因此内核返回的是真实
  路径，而非合成的路径）。handler 在 guest `execve` 时用它
  重新 exec 自身。
- **`guest_exe_path`** —— guest *请求*我们 exec 的路径，
  由 `--exec-child` 在转换 guest 请求的
  二进制路径之后设置。供 `/proc/self/exe` 合成 handler
  （§"`/proc/self/exe`"）返回 guest 所期望的值。

这是两个不同的静态全局变量。把它们弄混是很容易犯的 bug：
`our_binary_path` 是指向 `nativeLibraryDir` 的宿主路径；
`guest_exe_path` 是相对 rootfs 的路径，如 `/usr/bin/bash`。

Kotlin 安装侧在每次进入时重建 argv，与
为 proot/chroot 重建 bind 表的方式相同（单入口不变量见
`notes/rootfs-sessions.md`）。

