# tawcroot — SIGSYS handler

## Android 上的 robust-list 兼容

原生 `set_robust_list` / `get_robust_list` 在被允许时仍是内核操作。
在 Android 返回 `ENOSYS` 时，tawcroot 保存真实的每线程注册信息，
并支持查询调用线程（pid 0 或其 tid）。这让客户端可以检查
glibc 的链表，而不依赖 libc 私有的 TLS 偏移。查询其他线程
仍不支持。该模式**不**实现持有者死亡时的自动清理
或唤醒；线程在持有此类锁时死亡，可能使其他线程一直阻塞。
有界注册表在耗尽时回收已死线程的条目。不新增对 exit 系统调用的
拦截，因为 musl 可能在发出 exit 之前就解除其栈的映射。

需通过 Android 派生的进程测试：ADB/run-as 不继承 Zygote 的应用
seccomp 策略，无法确立与生产启动路径的兼容性。

## SIGSYS handler

热路径。只要 seccomp 过滤器返回
`SECCOMP_RET_TRAP` 就会被调用。签名：

```c
static void sigsys_handler(int sig, siginfo_t *info, void *ucontext);
```

`info->si_code == SYS_SECCOMP` 用于区分 seccomp 产生的
SIGSYS 和内核故障产生的 SIGSYS（如果不是 seccomp 我们就 abort ——
说明出了严重问题）。

`info->si_syscall` 和 `info->si_arch` 给出系统调用号和
架构（每个构建只会见到一种架构，但仍断言其
匹配）。参数位于 `ucontext` 内保存的寄存器状态中，
`ucontext` 的类型是 `ucontext_t *`。

handler 结构：

```c
{
    syscall_no = info->si_syscall;
    args = read_args_from_ucontext(uc);   // arch-specific
    pc   = read_pc_from_ucontext(uc);

    rv = tawcroot_dispatch_call(args, uc); // table lookup + handler,
                                           // wrapped by the lazy
                                           // DAC-override rescue

    write_return_value_to_ucontext(uc, rv);
    // ucontext is restored by the kernel on sigreturn
}
```

`dispatch_table` 是按系统调用号索引的固定函数指针
数组。大多数条目为 `NULL`。对 NULL 条目我们
返回 `-ENOSYS` —— **而不是** abort。Android 叠加的
`untrusted_app` 过滤器可能 TRAP 我们不处理的系统调用（在叠加的过滤器之间
TRAP 胜过 ALLOW；见 §"Seccomp 过滤器"），而
未来的 Android 版本可能 TRAP 更多系统调用。遇到
意外的 TRAP 就 abort 会使我们对这些变化很脆弱。
`-ENOSYS` 是内核对不支持的系统调用返回的值，
也是程序据以优雅回退时检查的值。

调用经过 `tawcroot_dispatch_call`（`rescue.c`），而不是
直接查表：它占用一个每线程的 rescue 槽位，运行
handler，并且 —— 仅当 handler 在虚拟 euid
0 下返回 `-EACCES` 时 —— 沿转换器
记录的路径放宽应用拥有的 inode，然后重新运行一次 handler。见
docs/tawcroot/path-translation.md §"DAC override"；在其他所有
结果下它就是单纯的查表：路径只有在第一次运行返回 `-EACCES` 之后，
通过重新运行一次 handler 才会被记录。

### 访问 guest 内存

guest 的指针位于我们的地址空间中，但它们仍是
不可信的系统调用参数。错误的路径指针、未终止的字符串
或无效的输出缓冲区，必须返回与内核相同的错误
（`-EFAULT` 或 `-ENAMETOOLONG`），而不是在
SIGSYS handler 内部让 tawcroot 崩溃。

因此所有对 guest 指针的访问都经过小巧的 `copy_from_guest`、
`copy_string_from_guest` 和 `copy_to_guest` 辅助函数。它们
在 handler 中安全且不依赖 libc。优先对自身任务 id 使用原始的 `process_vm_readv` /
`process_vm_writev`：内核会校验
guest 地址并返回 `-EFAULT`，而不会向
tawcroot 投递 SIGSEGV。这些辅助函数构建小的栈上 `iovec`，并通过
`tawcroot_raw_syscall()` 发出系统调用。绝不要直接对 guest 指针
使用 `memcpy`/`strlen`。

对自身执行 `process_vm_*` 总能通过内核的访问检查
（`__ptrace_may_access` 对同一线程组会在
Yama/SELinux 运行之前短路），且 Android 的应用 seccomp 策略允许它，因此
生产环境不需要兜底；如果探测失败，`supervisor_init` 会快速失败（exit 92）。
`-EPERM` 的探测失败意味着*宿主*上存在 seccomp errno
过滤器 —— 例如没有
`CAP_SYS_PTRACE` 的容器沙箱（Docker 的默认 profile）—— 宿主测试
harness 会检测到这一点并给出一条明确信息退出，而不是
产生一连串 `-EFAULT`。曾考虑过基于 SIGSEGV 修复的兜底拷贝路径，
但被否决：tawcroot 只虚拟化 SIGSYS，接管
guest 自有的 SIGSEGV（JIT 会用到，例如 Firefox wasm）
对一个生产环境从不会遇到的情形来说不值得。如果将来
真的需要兜底（沙箱化的宿主），优先用管道技巧 —— `write(pipe_wr,
guest_src, n)` 能获得内核地址校验，干净地返回 `-EFAULT` 和
部分计数，再读回即可完成拷贝 —— 而不是玩信号的花样。

代价：每次拷贝一次系统调用。传给 `process_vm_*` 的 pid 是
tgid（线程组中任一线程都能寻址共享的 VM），缓存在一个
`MADV_WIPEONFORK` 页上：fork 出的子进程读到 0，回退为
每次调用 `gettid`，直到被拦截的 clone 子进程路径重新填充它
（`tawc_usercopy_refresh_pid`）。绝不要在读到 0 时惰性填充 —— vfork
子进程共享该页，会在其 execve 之后留下自己的 pid。
僵尸 leader（主线程已退出，兄弟线程仍存活）
返回 `-ESRCH`，此时按 tid 重试。字符串读取取 512 字节
的块，在 4 KiB 边界处拆成两个远端 iovec，因此
恰好在未映射页之前结束的字符串会以部分读取返回，
而不是整个块都 `-EFAULT`。

这些辅助函数用于：

- 每个带路径的系统调用上的路径字符串；
- `readlink`/`readlinkat`、`getcwd` 及其他输出缓冲区；
- `execve`/`execveat` 的 argv 和 envp 指针数组及字符串。

在 handler 代码中，不要对 guest 提供的地址使用普通的 `memcpy`、`strlen`
或临时的指针遍历。

一个刻意的例外：热点 stat 路径（`fstat`、路径形式和
`AT_EMPTY_PATH` 形式的 `newfstatat`/`statx`）把 guest 的输出缓冲区交给
内核，然后就地修补伪造的字段（uid/gid、模拟硬链接
计数、gamepad 节点）（`finish_stat_direct`）。内核
校验指针并像原生调用一样返回 EFAULT；
handler 只在该写入成功之后才触碰缓冲区，因此它是
已映射且可写的。剩余风险是同一 guest 的兄弟线程
在中间解除缓冲区映射 —— 即 guest 与自己的系统调用竞争。这
为每次 stat 省下一次 `process_vm_writev`。影子 stat（shm、/proc）仍然
拷贝。由 `test_fs_handlers.c::hosted_stat_family_bad_out_pointer_efaults` 覆盖。

### 读写保存的寄存器

siginfo/ucontext 的形态与架构相关。我们将其隔离在
`arch/<arch>.h` 中，提供两个 inline 辅助函数：

```c
static inline void arch_read_args(ucontext_t *uc, syscall_args *out);
static inline void arch_write_return(ucontext_t *uc, long rv);
```

`x86_64`（`uc->uc_mcontext.gregs`）：
- 参数：RDI, RSI, RDX, R10, R8, R9 → `REG_RDI, REG_RSI, REG_RDX, REG_R10, REG_R8, REG_R9`。
- 返回值：RAX。
- pc：RIP。

`aarch64`（`uc->uc_mcontext.regs[]`）：
- 参数：x0..x5。
- 返回值：x0。
- pc：pc。

两种架构都需要（真机是 aarch64，模拟器是 x86_64）。
我们不需要处理 32 位 personality —— Android 的 lp32
过滤器存在，但 TAWC 全部是 lp64。

### 从 handler 发出宿主系统调用

我们不能直接调用 glibc 的 `syscall(2)`，因为 (a) glibc 的
`syscall` 可能经过 seccomp 过滤器会拦截的 vDSO/PLT 路径，
(b) 我们需要单一的、可加入 allowlist 的指令指针
范围。取而代之的是一个手写 stub：

```asm
; arch/x86_64/stub.S
.global tawcroot_raw_syscall
.type   tawcroot_raw_syscall, @function
tawcroot_raw_syscall:
    mov     %rdi, %rax           # syscall number → RAX
    mov     %rsi, %rdi           # shift args left
    mov     %rdx, %rsi
    mov     %rcx, %rdx
    mov     %r8,  %r10
    mov     %r9,  %r8
    mov     8(%rsp), %r9
.global tawcroot_raw_syscall_insn
tawcroot_raw_syscall_insn:
    syscall
.global tawcroot_raw_syscall_ret
tawcroot_raw_syscall_ret:
    ret
```

这里有三个相关的地址，混淆它们会破坏
整个设计：

- 位于 SYSCALL/SVC 指令**之上**的标签
  （`tawcroot_raw_syscall_insn`）是二进制中发生陷入的
  指令的地址。
- `seccomp_data.instruction_pointer` 和 `siginfo->si_call_addr`
  标识的是**系统调用之后的 PC** —— 即紧接在 SYSCALL/SVC
  *之后*那条指令的地址。Linux 用
  `pt_regs->ip` / `pt_regs->pc` 填充这两个字段，而系统调用入口汇编
  会把它们设为保存的返回 PC（下一条指令）。已在
  x86_64 和 aarch64 上实测确认 —— 见
  `tawcroot/src/main.c` 中的 smoke 驱动。SYSCALL 在 x86_64 上是 2 字节，SVC 在 aarch64 上是 4
  字节；系统调用之后的 PC 是 *insn 标签 + 指令
  长度*，与 `tawcroot_raw_syscall_ret` 是同一个地址。
- `ucontext_t` 中保存的程序计数器是恢复 PC；对于
  `SECCOMP_RET_TRAP`，它等于 `instruction_pointer`（handler 结束后
  从 `SYSCALL`/`SVC` 之后的指令继续执行）。

因此 BPF 过滤器检查
`instruction_pointer == &tawcroot_raw_syscall_ret`（**不是** `_insn`），并
只对这一个地址 `RET_ALLOW`。其他任何执行
SYSCALL/SVC 的都是 guest 代码，按正常的拦截表处理。

（在 aarch64 上等价实现使用 `svc #0`；同样是 `_ret` 标签模式。）

stub 符号（`tawcroot_raw_syscall`、
`tawcroot_raw_syscall_insn`、`tawcroot_raw_syscall_ret`）应设为
hidden（汇编中的 `.hidden` 指令），汇编函数放在
`.text` 中，不带特殊段属性。静态链接下
不用担心 PLT，但 hidden 可见性仍能确保
这些符号不会被 guest 之后 mmap 进来的代码插桩替换（一些
调试/ptrace 工具会这样做）。

**阶段 1 之前必须测试：** 一个小型 Android smoke 二进制安装一个
恰好只允许 `&tawcroot_raw_syscall_ret` 的过滤器，拦截来自其他位置的
普通系统调用，并在 x86_64 和 aarch64 上记录 `si_call_addr` 以及
`ucontext_t` 中保存的恢复 PC。如果
观察到的过滤器地址不是系统调用之后 PC 的标签，
就停下来先修复 stub/过滤器契约，再实现路径
转换。（状态：在 x86_64 模拟器上通过 —— 见
`tawcroot/tests/handler/test_foundation_smoke.c`、
`tawcroot/tests/testhost/src/smoke.c`，以及 `filter.c` 中
记录 `_ret` 与 `_insn` 差异的注释。）同一个 smoke 必须发出 handler
或 `--exec-child` 自举计划使用的每一个原始系统调用（`openat`、`read`、`write`、
`close`、`mmap`、`mprotect`、`munmap`、`getcwd`、`readlinkat`、
`fstatat`/`statx`、`fcntl`、`execveat`、状态 fd 的创建等）。
我们的 IP allowlist 只绕过我们自己的过滤器；它无法覆盖
Android 已经叠加的 zygote 过滤器。如果 Android 对 stub 发出的某个原始系统调用
`TRAP` 或 `KILL`，就在以其为基础构建之前换一个原语。

### 绝不发出比最老 Android 策略更新的系统调用

嵌套的 SIGSYS 是致命的，而且无声无息。我们的 `SIGSYS` handler 以
`sa_mask = ~0` 安装，因此运行期间 SIGSYS 被屏蔽；如果 handler 发出的
某个原始系统调用本身被 Android 的
zygote 过滤器 `RET_TRAP`，内核无法投递第二个 SIGSYS，
会强制杀死进程 —— 没有日志，没有退出码，没有 core。guest
就这么消失了。

由此得出的规则：**handler 只能发出我们支持的所有 Android 策略
都已允许的系统调用号。** Android 的应用
过滤器由设备出厂时 API 级别对应的 bionic 系统调用列表生成，
所以真正的判据是“对 `minSdk` 的 bionic 而言足够老”——
而不是“对内核而言足够老”。seccomp 在内核的未知系统调用检查
*之前*运行，所以新内核也救不了旧策略下的
新 NR。

已知实例，全部位于 `src/syscalls_fd.c`：

- `close_range`（NR 436，bionic API 34）：**模拟**，从不
  重新发出。`handle_close_range` 打开 `/proc/self/fd`，
  用 `getdents64` 遍历，并对范围内每个
  guest fd 执行关闭（或设置 `FD_CLOEXEC`），跳过保留表，反复重扫直到某一
  轮没有关闭任何 fd。当 `/proc` 无法打开时，回退为以
  `prlimit64(RLIMIT_NOFILE)` 为上界的线性循环。这是
  wmww/tawc#14：glibc 在 exec 前的 `closefrom` 在一台 Android 11 设备上杀死了每一次
  gpg/gpg-agent/dirmngr 派生，表现为
  pacman 的 "GPGME error: Invalid crypto engine"。
- x86_64 上的 `dup2` → `dup3`、`poll` → `ppoll`、`select` → `pselect6`、
  `epoll_wait` → `epoll_pwait`、`pipe` → `pipe2` 等：
  旧 NR 在 bionic 的 allowlist 中仅限 lp32，所以
  handler 发出现代的对应调用。
- `faccessat2`（NR 439）：被拦截并路由到 `faccessat`。

该规则的测试覆盖是合成的 android 过滤器
（`tawcroot/tests/handler/androidfilter/wrap.c`），它对
受版本限制的 NR —— 包括 436 —— `RET_TRAP`，因此任何重新发出其中之一的 handler
都会杀死 testhost，使整个模块失败。prod-env 层
（`tests/integration/tests/tawcroot_prodenv.rs`）是对应的
真实策略检查，但只覆盖目标 AVD 或手机所运行的
API。

### 为什么用 non-PIE

BPF 过滤器中基于 IP 的 allowlist 在安装时把
`&tawcroot_raw_syscall_ret` 作为固定地址字面量写入
过滤器程序。过滤器是内核状态，跨
`execve` 继承，并且**无法修改或移除** —— 只能在其上叠加新的
过滤器。seccomp 过滤器叠加遵循
“最严格者胜出”规则：`TRAP`（0x00030000）的优先级严格
高于 `ALLOW`（0x7fff0000）。只要栈中任一过滤器对某个系统调用
返回 TRAP，结果就是 TRAP，无论
其他过滤器怎么说。

这给重新 exec 架构带来一个硬约束：

1. 第一个 tawcroot 为 stub 安装一个 `IP == X → ALLOW` 的
   过滤器。`execve` 进入 guest 后，该过滤器被
   继承。
2. guest exec 某个程序 → handler 重新 exec tawcroot
   （`--exec-child`）。继承的过滤器仍然生效。
3. 如果 tawcroot 是 PIE，ASLR 会把 stub 放到新地址 Y。
   继承的过滤器写的是 `IP == X → ALLOW`，但 stub
   在 Y —— 不匹配 —— 继承的过滤器返回 TRAP。即使
   `--exec-child` 安装一个写着 `IP == Y → ALLOW` 的新过滤器，
   继承的 TRAP 也胜过新的 ALLOW。**stub 自己的
   系统调用被 TRAP。** handler 递归/死亡。

**使用 non-PIE**（`-static -no-pie`）时，二进制是 ET_EXEC。
内核每次都把它加载到链接期固定的地址。stub
在所有重新 exec 中位于同一虚拟地址。继承的
过滤器的 `IP == X` 检查与新进程的 stub 匹配。ALLOW。
无需新过滤器。

后果：
- `--exec-child` **不得安装新的 seccomp 过滤器。**
  继承的过滤器是正确的。它只重新安装 SIGSYS
  handler（被 `execve` 重置）并重新打开 rootfs fd / bind
  表。
- 选择避开 guest 正常映射
  区域的基址。使用 `-Wl,--image-base=<addr>`（NDK lld **不**
  支持 `-Ttext-segment` —— 它会报错并明确
  建议使用 `--image-base`）。基址：
  aarch64 为 **0x2000000000**（与 proot 的 `LOADER_ADDRESS` 一致），
  x86_64 为 **0x40000000**。aarch64 的数值可以很高，
  因为 `R_AARCH64_CALL26` 等重定位是 PC 相对的，且
  二进制内部自洽。x86_64 的数值被
  静态 bionic 的 `libc.a` 压到约 1 GB：它包含
  对弱未定义符号的 `R_X86_64_PLT32` 引用（例如
  `__cxa_thread_atexit_impl.o` 中的 `__loader_add_thread_local_dtor`、
  scudo 的 `flags.o` 中的 `__scudo_default_options`），静态
  链接把它们解析到地址 0。从我们的 `.text`
  到 0 的 PLT32 位移必须能放进有符号 32 位立即数，因此任何高于约 2 GB 的基址
  都会以 "relocation R_X86_64_PLT32 out of range." 链接失败。1 GB
  足以避开典型的 ET_EXEC 默认基址（0x400000），并且
  远低于 Linux/glibc ld.so 的加载位置（约 0x7f00_0000_0000），
  同时在 2 GB PLT32 上限之下留出 1 GB 余量。
- tawcroot 自身没有 ASLR。可以接受 —— tawcroot 不是
  安全边界（§"它明确不是什么"），而且 guest
  反正可以读 `/proc/self/maps` 找到我们的地址。

`PR_SET_SYSCALL_USER_DISPATCH` 在 seccomp 之外单独定义一个始终允许的 IP 范围，
但 Linux 5.11 是在 x86 上引入它的。
Linux 6.1 ARM64 缺少这一支持，所以仅靠更新的内核版本
并不能让它在 S24 上可用。它仍然使用 SIGSYS，且其
设置不会跨 fork/clone/exec 保留。见
[手册](https://man7.org/linux/man-pages/man2/PR_SET_SYSCALL_USER_DISPATCH.2const.html)
和[技术综述](execution-techniques.md)；它目前不是固定地址 BPF 路径
在 ARM64 上的替代方案。

### 线程与 `vfork` 不变量

handler 在调用线程的上下文中运行。有多个
guest 线程时，多个 SIGSYS handler 可以在
各自的栈上并发运行。这在构造上是安全的，因为：

- tawcroot 的核心全局变量（rootfs_fd、bind 表、知名符号链接
  memo 缓存、rootfs 宿主路径字符串）在进程生命周期中恰好被写
  两次：一次在 supervisor 初始化时，另一次只由
  `chroot(2)` handler 在 guest 发出 chroot 时写入。MVP
  刻意**不**维护可变的 guest cwd 字符串；
  `AT_FDCWD` 解析通过原始
  `getcwd` + 反向转换，从内核的当前 cwd 推导。fd 来源表和
  guest 可见的 `SIGSYS` 影子是明确的例外：固定大小、
  handler 安全，且只通过被拦截的控制类系统调用更新。
- 所有每次调用的状态都在栈上。
- 我们从不在 handler 中调用任何带隐藏可变状态的 libc 函数
  （不用 `getenv`，不读 `errno`，不用 stdio）。

chroot 路径变更竞争。`chroot` 是唯一在初始化后
重写核心全局变量的系统调用。写入是有序的（先把
长度清零，再写字节，再重新发布长度；bind 先停用再清除；
最后写入新 fd），因此并发读者只会落入
两种状态之一：看到旧的一致视图，或看到一个
“长度为零”/“没有 bind”的中间状态，每个读者都把它解释为
“路径在 rootfs 视图之外 → -ENOENT”。后者是
guest 在微秒内重试即可越过的瞬时 ENOENT。没有
显式内存屏障时，这在 aarch64 上不是硬保证，但
失败模式是有界的：并发读者最多在单个带路径的系统调用上
观察到一次瞬时 ENOENT 或错误路由。
对于我们面向的负载（pacman 6.x 的 chroot 在启动后是单线程的；
从工作线程中 chroot 的容器运行时
不在 Android 上运行），这是可以接受的。如果将来的负载需要
严格的多线程 chroot 安全，就切换到 seqlock 快照
协议（写者递增 seq；读者在看到奇数或
不匹配时重试）。

`vfork` 在 `execve` 之前与父进程共享 VM。运行我们 handler 的
`vfork` 子进程不得修改父进程依赖的普通全局变量。
chroot/init 例外之外的任何可变进程级状态
（fd 来源、guest `SIGSYS` 影子、将来的 namespace
跟踪）都必须使用专门设计的无锁快照方案，
保证对并发信号 handler 安全（例如，固定大小的
双缓冲状态加原子序列计数器），或者必须
每次调用都从内核状态重新推导。不要在没有 chroot 路径那种
有界失败分析的情况下，添加一个 handler 读取、而另一个线程
可能同时更新的普通可变 C 字符串/全局变量。

### guest 的信号/seccomp 控制

tawcroot 独占 `SIGSYS`。seccomp 过滤器是永久的内核状态，
整个架构假定对 seccomp 产生的陷入存在一个有效的 `SA_SIGINFO` handler。
guest 如果安装自己的 `SIGSYS`
handler、屏蔽 `SIGSYS`、把处置重置为 `SIG_DFL`，或
叠加另一个 seccomp 过滤器，即使它
“只是”在发出普通的 Linux 系统调用，也可能破坏 tawcroot。

因此这些系统调用是运行时接口面的一部分，而不是可选的
加固：

- `rt_sigaction`：虚拟化 `SIGSYS`。guest 读写
  `SIGSYS` 处置时看到的是 guest 影子值，而真实的
  内核处置仍是 tawcroot 的 handler。其他信号直接
  透传。如果 guest 对 `SIGSYS` 请求 `SIG_DFL`/`SIG_IGN`，就把
  它存入影子表，但不应用到内核。
- `rt_sigprocmask` / `sigprocmask`：将 `SIGSYS` 保留给转换使用。
  从请求的掩码中清除它，查询时报告其未被屏蔽。
  更新 `ucontext` 中保存的内核掩码，使 sigreturn 保留对
  其他信号的更改。没有每线程的掩码影子或 TID 表。
  不支持需要可屏蔽 SIGSYS 的程序；假装它
  已被屏蔽并不能提供那种行为。
- `sigaltstack`：原生执行；loader 公布信号栈预算。

- `seccomp(SECCOMP_SET_MODE_FILTER)` 和
  `prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, ...)`：接受但
  不安装过滤器。检查长度非空（最多 4096 条指令）
  且存储可读；这不是 BPF 校验器。seccomp 系统调用的标志
  只允许为零或 TSYNC。其他标志，包括 NEW_LISTENER，
  返回 EINVAL：无法提供通知 fd。其他 seccomp
  操作返回 ENOSYS。PR_GET_SECCOMP 报告宿主的真实状态。
  NULL 过滤器的能力探测报告不支持（seccomp：ENOSYS；
  prctl：EINVAL），而不是宣称我们并不提供的 BPF 强制执行。
  能够降级的应用不得进入依赖强制执行的路径。
  否则 guest 过滤器可能杀死原始系统调用 stub，或绕过路径名
  转换。因此本运行时不强制执行 guest 沙箱；
  只剩下 Android 应用边界，没有按项目/实例的隔离。

guest 可见的 `SIGSYS` 影子状态很小，但仍是可变的
进程状态。把它存放在固定大小的原子变量或与上述线程规则
兼容的快照结构中；不要在 handler 内部用
malloc 支撑的容器或 libc 锁保护它。

测试至少必须覆盖：guest `sigaction(SIGSYS, SIG_DFL)`、guest
屏蔽 `SIGSYS`、guest `prctl(PR_SET_SECCOMP)` / `seccomp(2)`（两者
都必须在安装过滤器时观察到成功，而实际上并未安装
过滤器），以及每次尝试之后的一次路径系统调用。预期结果
是路径转换仍然正常工作。

### 为什么 handler 是异步信号安全的

严格来说，`SA_SIGINFO` handler 只能使用
异步信号安全的 API。我们的 handler：

- 读取不可变的静态全局变量（rootfs fd、bind 表）以及
  固定大小的原子/快照运行时表（fd 来源、guest
  `SIGSYS` 影子）—— 没问题，只要这些表遵循
  上述线程规则。
- 通过 stub 发出原始系统调用 —— 没问题，`syscall(2)` 按定义就是
  异步信号安全的。
- 在路径经由受保护的 guest 拷贝辅助函数拷贝之后，
  遍历一个小的栈上字符串缓冲区。

我们**避免**的：malloc、stdio、locale、任何会加
锁的东西。路径缓冲区来自 `path_scratch.c` 静态池，而不是
handler 栈（见 §"Handler 栈预算"）。bind 表
在初始化时构建一次，之后不可变；少数可变的运行时
表必须无锁且 handler 安全。

由于 guest 的 glibc/ld.so 被手动加载到与静态链接 bionic 的 tawcroot
相同的地址空间中，handler/运行时还必须
被当作一个小型的独立（freestanding）运行时。用能阻止
编译器插入的运行时依赖泄漏进热路径的标志
编译 handler、guest 拷贝、路径、系统调用分发和架构 stub 对象
（`-fno-stack-protector`，不用 sanitizer，不做 profiling，
不假定 exceptions/unwind）。不要在
handler 中读取 bionic 的 `errno`；原始系统调用直接返回负的 errno 值。任何
在 handler 之外但在手动加载跳转之前使用的 libc 辅助函数，都必须
在静态 bionic 和继承的 seccomp 下显式审计。

我们刻意*不*设置 `SA_NODEFER`：内核在 handler 执行期间
屏蔽 SIGSYS。如果设置了 NODEFER，一个
本不应被拦截却被拦截的 stub 系统调用（例如 IP allowlist 判定
出现回归）会同步重入 handler，并在递归下
栈溢出；不设置时，第二个 SIGSYS 会挂起
直到外层 handler 返回，届时它触发
一次，我们要么成功，要么以单个嵌套
帧干净地崩溃。两种选择都说得通，但“默认不递归”
是更安全的一种，因为我们的 handler 状态只有不可变或快照
两类，而 handler 也没有真正需要重入自身的理由。

### Handler 栈预算

handler 运行在哪里分两档，因为 SIGSYS 以
`SA_ONSTACK` 注册：

- **线程有 `sigaltstack`** → 内核把信号帧，进而
  把我们整个 handler 调用链放在那里。这正是 Go 能工作的前提：
  goroutine 栈从 2 KiB 起步，运行时直接在其上发出系统调用，
  但每个 M 在 `runtime.minit` 中都会得到一个 32 KiB 的 altstack。
  没有 `SA_ONSTACK` 时，每个被拦截的系统调用会在
  2 KiB 栈之下写约 6 KiB，写进相邻的 goroutine 栈
  （Go 把它们紧密排列，所以不会触发故障），进程随后在
  GC 内部死掉 —— `micro`、`fzf`、所有 Go 程序（GitHub issue #6）。
- **没有 altstack** → 内核静默回退到发生陷入的
  线程自己的栈，其大小由 guest 的运行时决定。musl 的
  默认线程栈是 128 KiB；受支持的下限是由
  `static_small_stack_open_argv1` fixture 固定的 16 KiB（一个
  带显式 16 KiB 栈、执行带路径 open 的 clone 子进程）。我们永远不能
  *依赖* altstack —— 它是 guest 拥有的每线程状态 —— 所以
  这个下限和下面的帧上限仍然成立。

在 OnePlus 9（无 SVE）上的实测代价（污染 1 MiB，在区域中间 clone 一个子进程，
执行一次系统调用，扫描最低被覆写的字）：一次被拦截的
`getpid` 在 SP 之下写约 4.7 KiB，一次被拦截的带路径解析的 `openat`
约 6.2 KiB —— 大约 4.5 KiB 是内核的 `rt_sigframe`，另加约 1.4 KiB 的
handler 调用链。内核那一半会随 SVE（以及 x86_64 上的 xsave）增长。

`sigaltstack` 原生执行。loader 公布内核的
`AT_MINSIGSTKSZ` 加 16 KiB 的 handler 预算（较老的内核省略该辅助值时，
使用 16 KiB 的内核帧额度）。应用
必须提供足够的栈空间；不会替换固定的过小栈。
这保留了栈地址和元数据，并让调用者可以在解除栈映射后
立即禁用它，而不会有 SIGSYS 帧落在已释放的内存上。
由 `static_sigaltstack{,_small}_open_argv1` 和
`static_sigaltstack_swap_argv1`（替换，然后禁用 + munmap）固定。

预算是机械地强制执行的，而不是靠约定：每个
生产对象都以 `-Wframe-larger-than=1024 -Werror` 编译
（Makefile `TAWC_CFLAGS`，build.sh `COMMON_CFLAGS`），因此 handler 帧
超过 1 KiB 就是构建错误。规则（也写在
`include/stack_budget.h` 中）：

- 按 `PATH_MAX` 或 guest 控制的长度确定大小的缓冲区
  来自 `path_scratch.c` 池（BSS 中 128 槽 × 4 × 4096 B，
  无锁 CAS 获取），绝不放在栈帧中。
- 约 256 字节以下的固定缓冲区放在栈上没问题。
- 递归函数必须使每层帧尽量小 —— 上限会
  按深度相乘。最深的调用链是 exec 的 shebang
  分类（`classify_loadable`，深度 ≤ `TAWC_SHEBANG_MAX_DEPTH`
  = 4）；在 2026-08 修复之前，它每层约 2 KiB 的帧乘以五
  层，单这一项就可能逼近一个小的 guest 栈。
- 由锁串行化的较大暂存区放在静态变量中 —— exec 路径
  通过受 exec_lock 保护的静态缓冲区暂存（`exec_handler.c`）。
- 可证明从不在 guest 栈上运行的初始化和 `--exec-child` 自举代码
  （supervisor 初始化、过滤器安装、跳转之前的 loader）
  按文件/区域用 `TAWCROOT_FRAME_CAP_EXEMPT` 豁免。
  绝不豁免从分发表可达的任何代码。

testhost 构建编译同样的生产源码，外加在进程自身栈上
合理地使用大帧的 smoke 驱动，所以
它传入 `-Wno-frame-larger-than`；把关的是生产对象。

### Handler 编码约定

每个 fs-handler 都遵循的重构后约定（2026 年 6 月清理）：

- **单次 guest 读取。** handler 最多拷贝一次 guest 路径字符串
  （`fetch_guest_path` 到 scratch 池），针对这份本地副本
  对拦截项分类（`classify_shm`、`tawcroot_proc_shadow_open`、
  `tawcroot_is_proc_self_exe`），然后通过 `translate_local`
  转换*同样的字节*。分类之后绝不重新读取
  guest 指针 —— 竞争的 guest 线程可能在窥视与转换之间
  替换字符串（double-fetch）。
  没有拦截项的 handler 使用 `translate_at`，它把读取 +
  `translate_local` 合在一次调用中。
- **`struct fs_path`** 是转换输出的单位：`{fd,
  is_root, path}` —— 把 `fd`/`path` 直接传给 `*at` 系统调用；
  `is_root` 表示路径解析到了基目录本身，
  每个系统调用自行选择语义（多数用 "."，或 AT_EMPTY_PATH，
  或直接返回 errno）。
- **x86_64 旧系统调用是 `via_at` 垫片** —— 重建寄存器
  帧并重新进入现代的 *at handler（保留 nr，供
  依赖 nr 的 handler 使用）。不要为旧调用编写专门的实现。
- **有界字符串构建**通过 `tawc_str_append` /
  `tawc_str_append_dec` / `tawc_str_copy` / `tawc_proc_fd_path`
  （strings.c，有单元测试）—— 不手写有界拷贝循环。
- **被拦截并拒绝的系统调用**注册 `tawcroot_deny_enosys` /
  `tawcroot_deny_eperm`（dispatch.c），并在
  注册处写明理由注释。
- **/proc 影子合成**位于 `proc_shadow.c`；新增一个影子
  文件意味着一个分类器 kind、一个内容合成器，以及
  `tawcroot_proc_shadow_open` 中的一行。stat 接口面自动获得：一个
  分类器（`tawcroot_proc_shadow_classify`）供给全部四个 handler
  （open、stat、statx、access），元数据结果由
  kind 合成，绝不来自 memfd —— 每次 stat 都构建内容
  会为一次 `ls -l /proc/self` 重新读取并重写整个 maps 文件。
  **同步规则：**每个能 open 的 kind 也必须
  能 stat。handler 通过
  `proc_shadow_classify_at`（syscalls_fs.c）访问分类器，它也会通过
  fd 相对组合进行重试；如果新 kind 只落在一个接口面上，
  `hosted_proc_shadow_open_stat_lockstep` 会让 CI 失败。

### 系统调用点改写（`TAWCROOT_FASTPATH`）

在 aarch64 上默认开启（`include/fastpath_config.h`）；用
`EXTRA_CFLAGS=-DTAWCROOT_FASTPATH=0` 构建可关闭。stats 构建和
进程内测试编排器构建时关闭。`src/fastpath.c` 对热点 guest 系统调用点
跳过信号投递：

- SIGSYS handler 分发一次陷入后，若其 nr 在不需要 uc 的
  allowlist 上（身份 getter、`openat`、`close`、stat 族、
  `readlinkat`、`faccessat{,2}`、`getdents64`、`getcwd`），就对该点
  计数。第 64 次陷入时，若 `svc #0` 前一条指令是
  nr 相同的 `mov {w,x}8, #nr`，就把 `svc #0` 改写为 `b stub`。于是被修补点的 nr
  是静态的，`clone`、`rt_sigreturn` 之类
  永远不会经由它路由。
- 只修补位于 r-xp 文件映射内的点（依据 `/proc/self/maps`，
  有缓存）；为写入页面临时改为 RWX，然后恢复为 r-x。
  由于映射本来就是可执行的，这需要 SELinux execmem，
  而不是 execmod。stub 位于名为
  `[anon:tawcroot-fastpath]` 的 RWX 匿名区域，处于该点的分支跳转范围内。
- stub（`src/arch/aarch64_fastpath.S`）保存内核
  会保留的每个寄存器（x1–x30、NZCV、调用者保存的 SIMD），调用
  `tawcroot_fast_entry`，后者运行 `tawcroot_dispatch_call(args, NULL)`
  并返回到 site+4。
- 信号：SIGSYS handler 的 `sa_mask` 屏蔽所有信号，因此没有
  guest handler 能中断（或 `siglongjmp` 跳出）一个
  持有 scratch 槽位或 seqlock 写的转换。直接调用以同样方式屏蔽，
  但只在进程已经安装了 guest handler 之后
  （`handle_rt_sigaction` 报告每个非默认、非忽略的
  handler；该标志是粘滞的，因此 vfork 子进程的重置无法清除
  父进程仍持有的状态，而 exec 会开始全新的运行时）。在此之前
  每个处置都是默认或忽略，没有 guest 代码能在
  调用中途运行，所以两次 `rt_sigprocmask` 调用被跳过 ——
  这是 CLI 工具的常见情形。
- SVC 和 B 在架构上都可以在另一个
  线程正在执行它们时安全替换；stub 先写入并刷新。

S24，shell 域：使用无条件屏蔽时，`stat_many` 0.90、
`tar` 0.92、`find`/`ls -lR` 0.94、`grep -r` 0.96（相对默认
构建）；在 handler 存在之前跳过屏蔽，可在 stat/find/ls/tar 上再
乘以 0.90–0.93（`tawcroot/perf/results.md`）。
派生密集的循环不变（短进程在调用点达到
阈值之前就结束了）。`TAWCROOT_FASTPATH=2` 从不屏蔽：这是上限，且一旦存在
guest handler 就不再是可重入安全的。

阈值取 64，是因为每个进程都要为第一次修补付出代价（一次
`/proc/self/maps` 读取、两次 `mprotect`、一个 COW 页、一个 stub 区域）：
阈值为 4 时，ld.so 的库 `openat` 点在每个短生命周期
进程中都被修补，`true` ×100 / bash 启动慢了 4–6%；阈值为 64 时它们
不变，而长时间的遍历保留了收益。

已在应用中验证（`untrusted_app_34`，应用 seccomp 过滤器）：调用点
被修补，结果与不改写的构建一致，没有 AVC 拒绝，并且
bash 的 `trap` handler 仍能运行。`prod_rootfs_trapped_svc_preserves_registers`
检查改写前后的寄存器保留（来自同一调用点的 256 次调用；它能发现丢失的
NZCV；getuid 的路径不触及 SIMD，所以
无法发现丢失的 q 寄存器）。
