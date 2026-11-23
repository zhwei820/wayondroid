# tawcroot — 架构与进程布局

## 架构概览

一个进程，一个地址空间。三层，按依赖顺序：

```
+----------------------------------------------------------+
|  guest program (bash, pacman, firefox, …)                |
|  uses normal libc — its syscalls are intercepted          |
+----------------------------------------------------------+
|  trampoline / SIGSYS handler                              |
|  reads syscall args from ucontext, *emulates* the syscall |
|  via the trusted stub, writes only the return value back  |
+----------------------------------------------------------+
|  bootstrap                                                |
|  parses argv, builds seccomp filter, installs handler,    |
|  re-execs itself as --exec-child, then manual-loads guest  |
+----------------------------------------------------------+
```

"可信 stub"是位于已知地址的一小段代码，seccomp 过滤器通过指令指针检查对其
`RET_ALLOW`（精确判定条件见"从 handler 发出宿主系统调用"）。
过滤器把 stub 的系统调用指令地址列入白名单。handler 自身的所有系统调用都经由
该 stub 发出，因此过滤器不会递归。

### 为什么这里用单进程是正确的

在 ptrace 体系下需要两个进程，因为 tracer 的信号 handler 和内存无法从 tracee
访问。systrap 把这一点合并掉了：handler 运行在 tracee 线程内，可以完全访问
自己的数据段、堆以及保存的寄存器帧。不需要编组，不需要 `process_vm_readv`，
不需要 `PTRACE_GETREGS`。读 `info->si_call_addr`，写
`uc->uc_mcontext.gregs[…]`，返回即可。

这也修掉了 proot 最长期存在的 bug 之一："tracer 正在处理 tracee 的系统调用时
tracee 执行 fork"的竞态。没有 tracer，也就没有竞态。

## 进程布局

一次 `tawcroot` 调用就是一个 Linux 进程。其生命周期：

1. **解析 argv** — `tawcroot [-r <rootfs>] [-w <cwd>] [-b <src>:<dst>]… -- <cmd> <args…>`。
   顶层入口（非 `--exec-child`）还会设置 `PDEATHSIG`，并把 SIGHUP 重置为默认：
   Android 应用从 zygote 继承的 SIGHUP 是被忽略的，这会让每个 guest 都对终端挂断
   免疫。`--exec-child` 保留 guest 自己设置的处理（`nohup`）。
2. **打开 rootfs**，使用 `O_PATH | O_DIRECTORY`，并把 fd 存入全局状态。所有路径
   转换都通过该 fd 经由 `*at` 系统调用解析（不做字符串拼接，没有 TOCTOU 窗口）。
3. **构建 bind mount 表** — 每个 `-b src:dst` 变成一个预先打开的 `src` 的
   `O_PATH` fd，加上 `dst` 在 rootfs 内的路径。查找时按最长前缀匹配优先。
4. **设置 `PR_SET_NO_NEW_PRIVS`**。
5. **安装 SIGSYS handler**（`SA_SIGINFO`，不带 `SA_NODEFER` —
   见下文"为什么 handler 是异步信号安全的"）。
6. **安装 seccomp 过滤器**，通过 `seccomp(SECCOMP_SET_MODE_FILTER, …)`。
7. **经由 exec handler 路径启动 guest**：以 `--exec-child` 身份 re-exec 自身，
   重新安装 handler，然后手动加载 guest ELF + 解释器并跳转到其入口。
   不直接 `execve` 进入 guest，因为已捕获的信号 handler 会在 `execve` 时被重置
   （见 §"execve 处理细节"）。

第 7 步之后，进程在 tawcroot 地址空间中运行 guest 代码。handler、bind mount 表
和 rootfs fd 都位于 tawcroot 的数据段中，每当 TRAP 触发时由 handler 引用。

**FD 继承规则。** tawcroot 自己打开的能力 fd（`rootfs_fd`、bind 源的 `O_PATH`
fd、`our_binary_fd`）都是 `O_CLOEXEC`。它们会被 re-exec 进自身的 `execveat`
清除，`--exec-child` 在初始化时根据序列化状态重新打开它们。guest 自己的 fd
（stdio，以及它打开的任何东西）*不*归 tawcroot 标记 — 它们原样透传，由 guest
自己的 `O_CLOEXEC` 约定决定哪些存活。重新打开 tawcroot 的 fd 的代价：每次
guest exec 约 12 次 `openat`，远低于一微秒。

**内部 fd 保护规则。** 在手动加载跳转之后，仅靠 `O_CLOEXEC` 不够。此时 guest
代码与我们共享 fd 表，可以通过普通 fd 系统调用（`close`、`close_range`、
`dup2`、`dup3`、`fcntl`、`/proc/self/fd` 等）看到、关闭、dup 覆盖或检查任何
tawcroot 能力 fd。许多运行时还会在 fork 后主动关闭所有未知 fd。因此 tawcroot
必须把自己的 fd 当作保留 fd 区间对待，而不只是普通全局变量。

实现契约：

- 初始化时把所有 tawcroot 持有的 fd 放到一个高位基址或以上
  （`TAWCROOT_RESERVED_FD_BASE`，通过 `fcntl(F_DUPFD_CLOEXEC, base)`），并把
  编号记录在 `tawcroot_reserved_fds[]` 中。该基址是放置下限和 BPF 快速路径下限，
  **不是**我们独占的区间：guest 打开超过一千个 fd 后会合法地持有基址以上的 fd，
  所以每次判定都是精确的成员测试。两个集合天然不相交 — 内核永远不会分配一个
  已打开的 fd 编号。
- 拦截能影响任意 fd 编号的 fd 控制类系统调用：`close`、`close_range`、`dup`、
  `dup2`、`dup3`、`fcntl`，以及存在时的 `pidfd_getfd`。guest 针对内部 fd 的操作
  必须表现为该 fd 不存在（`-EBADF`），除非调用是 tawcroot 自己通过
  `tawcroot_raw_syscall()` 发出的。
- `close_range` 是**模拟**的，从不重新发出：handler 用 `getdents64` 遍历
  `/proc/self/fd`，对区间内每个 guest fd 执行关闭（对 `CLOSE_RANGE_CLOEXEC`
  则执行 `F_SETFD`），跳过保留 fd。在 API 34 以下的 Android 上，从 handler
  重新发出 NR 436 是致命的 — 见 [sigsys-handler.md](sigsys-handler.md) 中的
  "绝不发出比最老 Android 策略更新的系统调用"。早期的做法是
  围绕保留 fd 拆分区间（每个间隙仍发一次原始 436），或者把 `last` 截断到基址，
  后者会悄悄地让 guest 自己的高位 fd 保持打开。
- `dup2`/`dup3` 拒绝显式指定为保留 fd 的 `newfd`（`-EBADF`）；
  `fcntl(F_DUPFD*)` 完全不需要防护，因为内核只会返回空闲编号。
- tawcroot 不再使用的编号必须先从表中移除，再关闭其 fd，否则我们最终会对一个
  被回收复用的 guest 描述符撒谎。
- 在加入 proc 合成后，从 `/proc/self/fd` 和 `/proc/self/fdinfo` 视图中隐藏
  内部 fd。在此之前，测试至少要覆盖：guest 打开或关闭内部 fd 编号时无害地失败，
  且不会破坏后续的路径转换。

guest fd 来源表是"全局变量不可变"这一简化中唯一的 MVP 例外。它必须是一个
定长的、handler 安全的表，只由被拦截的创建 fd / 销毁 fd 的系统调用更新，采用
原子槽位发布或其他无锁方案。不要在这条路径上使用 cleat/STC 容器或基于 malloc
的映射。

这是正确性要求，不是加固表演：执行 `close_range(3, ~0U, 0)` 的 shell 或语言
运行时，绝不能把下一次转换后的 `openat` 所依赖的 rootfs fd 销毁掉。

**re-exec 状态交接。** `O_CLOEXEC` 规则的唯一例外是一个短生命周期的
`exec_state_fd`，由 `execve` handler 在 re-exec tawcroot 之前立即创建。它作为
`--exec-child` 之后的位置 argv 槽传递（即 `argv[2]`，格式化为十进制整数），
并且针对这一次宿主 exec 有意*不*设置 `FD_CLOEXEC`。该 re-exec 的 `argv[0]`
不是 "tawcroot"，而是序列化的 proctitle（以空格连接的 guest 命令行加上 shebang
余量）：它决定内核参数区的大小，使加载器能在跳转前原地把它覆盖为真正的、以 NUL
连接的 guest argv，从而让每个 guest 进程的 `/proc/<pid>/cmdline`（以及通过
`PR_SET_NAME` 设置的 `comm`）在内核层面真实可信 — 见 `include/proctitle.h`。
入口分类只读取 `argv[1]`/`argv[2]`。`--exec-child` 通过
`tawcroot_raw_syscall()` 读取它，重建进程状态，然后在手动跳转到 guest 代码前
关闭它。该 fd 对 guest 永远不可见。

exec 参数字符串使用由内核软 `RLIMIT_STACK` 推导出的 argv/envp 共享预算：取其
四分之一，上限 6 MiB，下限 32 页。每个字符串（含 NUL）限制为 32 个宿主页。
指针存储和可执行文件路径在提交前计入预算。收集阶段使用临时匿名映射；序列化
按状态写入器报告的精确大小分配。两个映射都在 re-exec 前释放，出错时也一样。
SIGSYS 中不运行任何 libc 分配器。带版本的状态格式仍把条目数限制为 4096 个参数
和 1024 个环境变量条目；这并非 Linux 的完整条目数范围。

状态 v9 还携带一个固定的可执行文件 fd。prepare 阶段只打开并校验可执行文件一次，
清除该私有描述符上的 CLOEXEC，新的加载器消费并关闭它。prepare/commit 失败时
也会关闭它。这对源 fd 为 CLOEXEC 的 `execve("/proc/self/fd/N", ...)` 很重要：
源 fd 会在内部 re-exec 时被关闭，但可执行文件必须仍可加载。在 commit 之后重新
打开原路径并不等价。

状态格式带版本且以长度为前缀的二进制格式，不是临时拼凑的字符串解析。它包含：

- rootfs 宿主路径；
- bind 规格（`src` 宿主路径，`dst` guest 路径），保持原始顺序；
- 当前根状态（目前为空；未来嵌套 `chroot` 会加入内层根前缀）以及一个可选的、
  用于诊断的 guest cwd 提示；
- tawcroot 标志，例如调试日志；
- guest 请求的 exec 路径和 `argv` 字符串。

guest `envp` 确实会被复制进状态 fd（加载器把它放到合成的 guest 栈上），handler
同时也把它作为 `execveat()` 的 envp 传入 — 即指向 mmap 后状态的指针 — 这样内核
构建的环境区（`/proc/<pid>/environ`、`ps e`）显示的是 guest 环境而不是空。两种
视图来自同一份序列化字节，因此一致；以状态为准。re-exec 后的 tawcroot 把该环境
纯粹当作 guest 负载；`--exec-child` 不从中读取内部配置。

合成的栈是一块新的 8 MiB 映射，而不是内核的 `[stack]`，因此 `/proc/self/stat`
的 startstack（第 28 字段）被改写为 guest 的初始 SP
（`tawcroot_guest_start_stack`）：bionic 依据它确定主线程栈的大小
（`pthread_getattr_np`），ART 也在那里放置栈保护。

首选实现是 `memfd_create("tawcroot-exec-state", MFD_CLOEXEC)`，随后只在传给
`execveat` 的那个 fd 上清除 `FD_CLOEXEC`。如果目标上 `memfd_create` 不可用或
被阻止，回退到预先打开的 app 缓存目录 fd 加 `openat(O_TMPFILE)`，或者仅在小型
冒烟测试中使用管道。不要回退到用环境变量传递内部状态；原样保留 guest 环境是
契约的一部分。

**环境规则。** `--exec-child` 不查阅 `environ` 来获取 tawcroot 配置。内部状态
通过初始 CLI 传递，或在 re-exec 时通过 `--exec-child` 的 argv 中的位置
`<state-fd>` 槽传递。这使"envp 原样透传"切实可行：子进程枚举 `environ` 只为了
把 guest 环境复制到合成的 guest 栈上，而不是解读 tawcroot 设置。
