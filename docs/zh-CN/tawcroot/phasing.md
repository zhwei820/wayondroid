# tawcroot — 阶段划分

## 阶段划分

- **阶段 0 — 基础冒烟**：✓ 已在 x86_64 模拟器（内核 6.6，Android 16）
   上完成。静态 non-PIE 二进制、原始系统调用 stub、针对
   `tawcroot_raw_syscall_ret`（不是 `_insn` —— 内核报告的是系统调用之后的
   PC；见 §"从 handler 发出宿主系统调用"）的 BPF IP 白名单、
   跨自身 exec 继承的 seccomp、通过 memfd 传递的非 `CLOEXEC`
   `exec_state_fd`、SIGSYS handler 重新安装，以及在我们的冒烟过滤器下
   通过 stub 演练原始系统调用集合。aarch64 交叉构建无问题。遗留：真实
   Android `untrusted_app` zygote 过滤器验证（需要 APK 级部署 —— `run-as`
   不继承 zygote 过滤器；并入阶段 4）。
- **阶段 0.5 — 运行时不变量保护**：✓ 已在 x86_64 模拟器（Android 16，
   内核 6.6）上完成；aarch64 交叉构建无问题。
     • 内部 fd（`rootfs_fd`、bind 的 src_fd）在初始化时通过
       `fcntl(F_DUPFD_CLOEXEC, base)` 保留在
       `TAWCROOT_RESERVED_FD_BASE = 1000` 及以上。从 guest 视角看，
       每个 ≥ 1000 的 fd 都返回 -EBADF。（此后已被取代：整个上半区间
       本就不归我们占用，占用它会导致持有这么多 fd 的 guest 出错。现在
       只拒绝已记录的 fd —— 见 `include/fdtab.h`。）
     • 拦截 + 处理：`close`、`close_range`（在保留边界处截断，使
       `close_range(0, ~0u)` 只关闭 guest 可见的 fd —— 后来改为在保留 fd
       周围拆分，现在则基于 `/proc/self/fd` 完全模拟，因为 API 34 以下的
       Android 策略会对 NR 436 RET_TRAP）、`dup`、`dup2`（仅 x86_64；
       aarch64 只有 dup3）、
       `dup3`、`fcntl`（当时会把 F_DUPFD/F_DUPFD_CLOEXEC 的最小值上限设为
       base-1；现已不再如此）。
     • SIGSYS 影子：`rt_sigaction(SIGSYS, ...)` 在 guest 侧的
       `struct kernel_sigaction` 影子上往返 —— 内核中的处置方式始终是
       我们的 handler。其他信号的 `rt_sigaction` 原样透传。
     • 信号掩码影子：`rt_sigprocmask` 在转发给内核前从任何新掩码中去掉
       SIGSYS 位，并根据影子标志 `g_guest_sigsys_blocked` 把它 OR 注入
       oldset。guest 读回的就是它写入的；内核从不阻塞 SIGSYS。
     • seccomp 拒绝：`seccomp(2)` 和 `prctl(PR_SET_SECCOMP)` 返回
       -EPERM；`prctl(PR_GET_SECCOMP)` 及其他 prctl 操作透传。
     • 严苛测试：guest 执行 `close_range(3, ~0u)`、
       `rt_sigaction(SIGSYS, SIG_DFL)`、`rt_sigprocmask(SIG_BLOCK,
       {SIGSYS})` 以及 `seccomp(SET_MODE_FILTER, ...)` 之后，经由我们
       handler 的 openat 仍然在 rootfs 内解析。
     代码位于 `src/syscalls_fd.c` + `src/syscalls_control.c`；
     保留入口是 `include/fdtab.h` 中的 `tawcroot_fd_reserve`。guest 多线程
     的掩码状态（每线程影子）是阶段 2 的后续工作 —— rootfs 系统调用冒烟
     测试是单线程的。
- **阶段 1 — MVP 路径转换（宿主侧）**：✓ 已在 x86_64 模拟器（Android 16，
   内核 6.6）上完成，**并已在 aarch64 设备**（OnePlus 9，Android 14，
   内核 5.4.284）上通过 `tawcroot/test.sh --device` 验证；Android 过滤器
   相关的覆盖由 `tests/handler/androidfilter` 合成。
   路径转换、fd 保留、SIGSYS/sigprocmask 影子、seccomp/prctl 拒绝、
   常见符号链接 memo、fake-root 身份、`renameat2`、`truncate`，以及区分
   模式的 lstat-vs-stat memo 全部在设备上通过。随着这次运行，阶段 0 遗留的
   "真实 `untrusted_app` zygote 过滤器验证"一项也已关闭。
   `faccessat2`（内核 ≥5.8）和 `close_range`（内核 ≥5.9）的
   handler 套件用例在 5.4 上被跳过 —— testhost 从原始系统调用检测到
   -ENOSYS 并输出 `[skip]`（由 `tawcroot/tests/handler/steps.c` 解析，
   记为通过）。对 `close_range` 而言此后已被取代：handler 现在基于
   `/proc/self/fd` 模拟 NR 436（必须如此 —— Android 在 API 34 以下会拦截它），
   因此这些用例与内核版本无关，跳过分支也已删除。
   polyfill 仍是按系统调用逐个选择启用，而不是通用策略。
   完整的 handler 集合：
     • argv 解析（`-r <rootfs>`、可重复的 `-b src:dst`）
     • 分发表；BPF 拦截集合由同一份 handler 列表生成
     • 按架构的 ucontext 胶水代码（读取参数、写回返回值）
     • 带 `..` 钳制的绝对路径转换；逃逸尝试
       （`/../../host-secret`）可证明地被钳制在 rootfs 根
     • 通过原始 `getcwd` + 去除 rootfs 宿主前缀，对相对路径做反向转换
     • 最长前缀匹配的 bind mount 表
     • 常见符号链接 memo 化（`/lib`、`/lib64`、`/bin`、
       `/sbin`、`/usr/sbin`、`/usr/lib64`、`/var/run` —— 典型的
       glibc rootfs 符号链接命中集合）—— 任何程序启动时 ld.so 打开库
       都需要它
     • fake-root 身份（`getuid`/`geteuid`/`getgid`/`getegid` → 0）
     • 元数据修饰：`fstatat` 和 `statx` 的 `st_uid`/
       `st_gid`/`stx_uid`/`stx_gid` 改写为 0
     • 携带路径的 handler：`openat`、`readlinkat`、`faccessat2`、
       `chdir`、`getcwd`（反向转换）、`mkdirat`、`unlinkat`、
       `symlinkat`、`linkat`（按 proot 的 `--link2symlink`，在
       EACCES/EPERM 时回退为符号链接）、`fchmodat`、`fchownat`
       （fake-root 下无操作并返回成功 —— 宿主 uid 无法 chown rootfs
       文件，磁盘上的属主仍是 app-uid）
     • 旧式 x86_64 包装（`stat`/`lstat`/`access`/`readlink`/
       `chmod`/`chown`/`lchown`/`mkdir`/`rmdir`/`unlink`）转到
       *at 变体处理 —— 这个权宜之计顺带补上了 Android 在 x86_64 上
       lp64-`access` 的缺口，作为路径转换的副作用。同理，x86_64 上还有
       `poll`→`ppoll` 和 `epoll_wait`→`epoll_pwait`：Android 的
       untrusted_app 过滤器对旧式变体 RET_TRAP，所以我们直接转发。没有
       `epoll_wait` 垫片时，mio 的 epoll 后端（wezterm 使用，可能还有
       其他程序）会从空的分发槽得到 -ENOSYS，并以
       "polling for events: ENOSYS; terminating" 中止。
     • `ioctl` `TCGETS2`/`TCSETS{,W,F}2`：先尝试原生调用，在 -EACCES
       时回退到旧式 `TCGETS`/`TCSETS{,W,F}` 系列。
       Android 的 `untrusted_app_all` sepolicy `allowxperm` 集合
       `unpriv_tty_ioctls` 白名单包含旧式四个，但至少在 Android 15
       模拟器上不包含 termios2 变体，因此内核 SELinux 关卡对每次
       TCGETS2 都返回 -EACCES。glibc 的 `tcgetattr` 先发 TCGETS2，
       只在 -EINVAL 时回退，-EACCES 时不回退 —— 于是 `bash`、
       `lxterminal`、`wezterm` 都看到 EACCES，认定 stdin 不是 tty，
       跳过提示符和 readline（即"能渲染但没有提示符也无法输入"的症状）。
       回退绕开了 xperm 关卡；两种内核 ABI 结构体前 36 字节相同
       （4*tcflag_t + c_line + c_cc[19]），speed_t 尾部置零（对 pty
       负载无关紧要 —— c_cflag 中的 CBAUD 位原样携带符号化波特率）。
       采用先尝试原生而不是总是转换，是因为 xperm 缺口取决于 Android
       版本和厂商（OnePlus 9 正常支持 TCGETS2）；在宽松的策略下，我们
       希望拿到内核完整的 struct termios2，包括真实的
       c_ispeed/c_ospeed，而不是合成的旧式视图。其他错误
       （-ENOTTY、-EFAULT、-EINVAL、…）原样透传。
       其他所有 ioctl 号都经由 TAWC_RAW 透传给内核；对 `ioctl(2)` 的
       拦截是无条件的，因为在 BPF 中按 cmd 参数匹配比当前过滤器生成器
       支持的要复杂 —— 性能上，终端应用每帧发出 O(1) 次 ioctl，SIGSYS
       往返开销可以忽略。
     • 通过 `process_vm_readv` 做 EFAULT 安全的 guest 指针拷贝
       （初始化时用 `tawc_usercopy_init` 探测）；`openat(NULL)`
       和 `openat(<unmapped>)` 干净地返回 -EFAULT，handler 不会崩溃
     • 四种解析模式 API（FOLLOW / NOFOLLOW / PARENT_CREATE /
       PARENT_REMOVE）贯穿所有携带路径的 handler。
       常见符号链接 memo 器区分模式：仅含单个组件的匹配只在 FOLLOW 下
       改写，因此 `lstat("/lib")` 返回符号链接的 `S_IFLNK`，而
       `stat("/lib")` 返回目标目录的 `S_IFDIR`。已在 x86_64 模拟器上验证。
     • `rename` / `renameat` / `renameat2`（双路径转换，
       使用 PARENT_REMOVE + PARENT_CREATE），`truncate`（转换路径，
       然后 `openat` + `ftruncate` + `close`）。旧式 x86_64
       `link`/`symlink`/`rename` 转到对应的 `*at` 版本处理。
     • 在内核 ≥5.6 上通过带 `RESOLVE_IN_ROOT` 的 `openat2(2)` 做通用的
       非末尾组件符号链接解析。openat handler 在初始化时探测
       （`tawcroot_path_probe_openat2`），可用时走 openat2，通过把解析
       重新定根到我们的 rootfs fd，让内核处理任意 rootfs 内符号链接（包括
       否则会逃逸的绝对目标）。已验证：一个含
       `/etc/host-secret → /etc/passwd`（绝对路径）的伪 rootfs 打开结果为
       ENOENT（被钳制），而不是泄露宿主的 `/etc/passwd`。
     • 手动的符号链接感知规范化（`src/path_resolve.c`
       + `src/path_fold.c`，oracle 接口在 `include/path_oracle.h`）。
       逐组件遍历，调用文件系统 oracle 的 `readlink`，把目标拼接回剩余
       后缀，重新 fold，并以 `SYMLOOP_MAX = 40` 为遍历上限。区分模式：
       NOFOLLOW / PARENT_CREATE / PARENT_REMOVE 不处理末尾组件。它在
       `tawcroot_path_translate` 中于 fold + memo 之后、bind 路由之前
       无条件运行，因此所有携带路径的 handler（不只是 `openat`）在任何
       内核上都获得同样的钳制规则。在 `path.c` 中标记为 `LEGACY-5.4`：
       该解析器之所以存在，是因为内核 <5.6 没有
       `openat2(RESOLVE_IN_ROOT)`，并且被良好隔离以便将来删除
       （`include/path_resolve.h` 中的横幅注释记录了删除步骤）。
       两种方式测试：基于 mock oracle 的 cleat 单元测试
       （`tawcroot/tests/unit/test_path_resolve.c`：链、自环、深度炸弹、
       NOFOLLOW 末尾组件、绝对目标钳制、`..` 目标钳制），以及在宿主 +
       设备上针对真实伪 rootfs 的端到端测试
       （`tawcroot/tests/handler/test_rootfs_syscalls_smoke.c` 的 rootfs 增加了
       `/altpath`、`/chain1..3`、`/loop`）。
   阶段 1 遗留：`handle_openat` 中的 openat2 快速路径为了 5.6+ 上的微小
   性能收益而保留，但现在已是冗余的 —— 解析器已经把路径规范化。之后可以
   低成本删除。
- **阶段 2 — execve 处理**：拆分为子阶段，因为工作是逐步落地的，而文档
   对完成情况的描述一直不实。
   原先"手动 loader 是最耗时的部分"的说法已经过时：loader 在宿主上对静态
   和动态 guest 均可端到端工作，带完整的 argv/envp/auxv
   （`tawcroot/tests/unit/test_loader_smoke*`、
   `tawcroot/tests/integration/test_prod_exec`、`tawcroot/tests/integration/test_exec_child`、
   `tawcroot/tests/integration/test_exec_via_handler`）。

   **子阶段 2a-2g 全部在宿主 x86_64 上通过。交叉构建（aarch64、
   x86_64）无问题。真实 Android 验证待阶段 4（APK 接入）。**

   子阶段：
   - **2a — 进程内 loader（`--exec`）**：✓ 已在宿主 x86_64 上完成。
     `tawcroot_loader_exec` 解析 ELF，映射 PT_LOAD（含 BSS 不完整页清零
     + 匿名扩展），读取 PT_INTERP 并为动态 guest 映射 ld.so，分配新栈，
     构建 argc/argv/envp + 完整 auxv（包括来自 `getrandom` 的
     `AT_RANDOM`），然后跳转。`/bin/true`、`/bin/ls /dev/null`、动态
     exit-42 均通过。
   - **2b — `--exec-child` memfd 交接**：✓ 已在宿主上完成。handler 把
     带版本的 exec_state 写入非 CLOEXEC 的 memfd；`--exec-child` mmap
     它，解析 path/argv/envp，交给 loader。
     静态 + 动态 guest 均可往返；bad-fd / 魔数损坏 / 缓冲区过短等情况都
     干净地返回。
   - **2c — SIGSYS handler 侧的 `execve` 拦截**：✓ 已完成。
     `tawcroot_exec_handler_perform` 构建 memfd，打开
     `/proc/self/exe`，并 `execveat` 进入 `--exec-child`。在 rootfs
     模式下，它现在会通过 `tawcroot_path_translate` 转换 guest 路径用于
     存在性探测，并把 rootfs+binds+guest_exe 记录到 exec_state 的可选
     extras（v2 格式）中，使 `--exec-child` 能重建状态。
     由 `prod_rootfs_guest_does_execve` 集成测试验证。
   - **2d — 生产 CLI `-r/-b/--` + loader 中的路径转换**：✓ 已完成。
     `tawcroot -r ROOTFS [-b SRC:DST]... -- CMD [ARGS...]` 在
     `main.c` 中解析 argv，以 O_PATH 打开 rootfs fd，将其保留到高位区间，
     构建 bind 表，安装 handler+过滤器，并调用 `tawcroot_loader_exec`。
     loader 检测到 `tawcroot_rootfs_fd >= 0` 时，会把 guest 二进制的路径
     以及 PT_INTERP 都经过 `tawcroot_path_translate`。由
     `tawcroot/tests/integration/test_prod_rootfs.c` 中的 8 个用例验证
     （rootfs 内的静态二进制、bind dst 路由、`..` 钳制、缺失/错误的参数
     形式、不可达的 guest）。
   - **2e — `/proc/self/exe` 合成**：✓ 已完成。
     `tawcroot_set_guest_exe_path`（在 `path.c` 中）暂存 guest 请求的
     exec 路径；`handle_readlinkat` 匹配 `/proc/self/exe` 和
     `/proc/<our-pid>/exe` 并返回暂存值。生产 `main.c` 在
     `prod_rootfs_init` 之后从 `argv[cmd_start]` 设置它；
     `tawcroot_loader_exec_child` 从携带的 `exec_state.guest_exe`
     重新设置（若不存在则用新的 exec 路径）。在 `rootfs_smoke.c` 中验证。
   - **2f — 在 `--exec-child` 中重新安装 handler**：✓ 已完成。
     当 `exec_state.rootfs_host` 存在时，`tawcroot_loader_exec_child`
     调用 `tawcroot_supervisor_init`（在 `src/supervisor.c` 中）。
     supervisor_init 依次：以 O_PATH 打开 rootfs、保留、设置宿主路径、
     usercopy 探测、添加 bind、重新注册继承的 shm 名称表、memo 化符号
     链接、dispatch_init、安装 SIGSYS handler、重置信号掩码、探测
     `openat2`、暂存 `/proc/self/exe`。seccomp 过滤器不重新安装（它作为
     内核状态被继承 —— 见 "为什么用 non-PIE"）。顶层入口的
     `prod_rootfs_init` 也调用同一个 supervisor_init，因此引导逻辑只在
     一处。没有这一步，exec 之后 guest 的第一个携带路径的系统调用要么
     被路由到宿主路径（没有 rootfs 视图），要么杀死进程（没有 handler）。
   - **2g — 多进程正确性**：✓ 静态 execve 已完成。
     `prod_rootfs_guest_does_execve` 验证完整链路：
     过滤器拦截 → handler 分发 → 写入带 extras 的 memfd →
     execveat 自身 → `--exec-child` 重新初始化 → 在 rootfs 内手动加载
     目标 → exit 42。`tawcroot/tests/integration/test_prod_fork.c`
     把覆盖面扩展到 guest 在执行会被拦截的系统调用之前先 **fork**
     （clone(SIGCHLD)，独立 VM 的子进程）的情形：
       * `prod_fork_child_opens_marker_in_rootfs` —— 子进程的 openat
         以不同于父进程的 PID/tid 演练路径转换，防止缓存 tid 的 bug 回归
         （usercopy.c:14-22；见下文"更多阶段 5b bug"）。
       * `prod_fork_then_execve_in_child`（+ `..._with_bind`）—— bash
         式的 fork+execve 链，从 fork 出的子进程 PID 演练 exec_handler
         以及 bind 表的重新导出。
       * `prod_fork_closefrom_then_execve_in_child`
         （+ `..._with_bind`）—— gpgme/closefrom 形态：子进程在 execve 前
         执行 close(1000..1003) + close_range(0, ~0u, 0)，捕获保留 fd
         存活方面的回归（见下文"阶段 5c — 完整集成测试套件"）。
       * `prod_fork_exec_proc_self_exe_correct_in_child` —— Firefox
         libxul.so 回归：后代进程的 /proc/self/exe 必须解析为它自己的路径，
         而不是最初的 guest 二进制
         （exec_handler.c:91-101；见"阶段 5c"）。
     `tawcroot/tests/integration/test_prod_features.c` 覆盖了以前只在
     rootfs-smoke testhost handler 层测试的生产二进制功能路径：
       * `prod_unix_bind_translates_sun_path` —— AF_UNIX bind() 的 sun_path
         转换（gpg-agent 回归）。此前在任何地方都没有测试。
       * `prod_proc_self_fd_hides_reserved` —— getdents64 目录项过滤器
         隐藏保留 fd 1000（gpgme closefrom 死循环）。
       * `prod_inherited_sigsys_block_unblocked_by_init` —— 编排进程
         阻塞 SIGSYS 后 fork 出 tawcroot；正是 supervisor_init 的
         SIG_SETMASK 重置让 guest 第一个被拦截的系统调用得以存活
         （JVM 派生 shell 的回归，"阶段 4"修复）。
     *动态* guest 的 fork+exec 以及 `bash -c 'ls'` 式的流程，仍需要一个
     rootfs 内带 glibc 的测试夹具，但以往回归实际触发所经过的静态路径
     现在已被覆盖。

   退出条件（原为"在伪 rootfs 内运行动态链接的 `/bin/true` 和
   `/bin/sh -c "ls /"`"）：部分满足。rootfs 内的静态二进制可端到端运行，
   包括 guest 发起的 execve。伪 rootfs 内的动态二进制需要一个包含动态
   链接器的夹具 rootfs —— 这是集成测试脚手架工作，与动态 fork+exec 的
   后续工作一并跟踪。
- **阶段 3 — 完整的拦截系统调用面**：上文 "哪些系统调用需要拦截"
   中的每个系统调用。
- **快速迭代循环（在真实 Android 目标上调试 tawcroot）。**
   完整安装流程（下载 + 解压 + pacman -Syu）约需 7 分钟；每次改代码都
   重来太慢。可用的迭代循环：

   1. 一次性准备：用 `--es method tawcroot` 安装 APK 一次，
      让它运行到 bootstrap 解压完成（PKG_KEYRING 阶段）。即使安装在
      pacman-key 处出错，`/data/data/me.phie.tawc/distros/arch/rootfs/`
      下的 rootfs 也是完整的。
   2. 每次迭代：`tawcroot/build.sh --abi=<abi>`，
      然后 `adb push app/src/main/jniLibs/<abi>/libtawcroot.so
      /data/local/tmp/tawc-dev/libtawcroot.so`，再 `adb shell 'su -c "cp
      /data/local/tmp/tawc-dev/libtawcroot.so <apk-lib-path>/libtawcroot.so"'`。
      总计约 3 秒。
   3. 测试：`adb shell 'run-as me.phie.tawc sh -c "cd <rootfs> && \
      <libtawcroot> -r <rootfs> <binds> -- <cmd>"'`。

   这样既绕过了 APK 安装，也绕过了 Arch 安装。只有 Kotlin 改动
   （TawcrootMethod 等）才需要完整重建 APK +
   `adb install -r`。

- **阶段 5 已在 x86_64 模拟器上完成。** 通过 app 内 `InstallActivity`
   进行的完整 `tawcroot` 方式 Arch 安装达到 `state: READY`，在宿主 shell
   上 `scripts/rootfs-run.sh "uname -a; id; pacman --version"` 输出干净的
   Arch 结果（`Linux localhost ...`、`uid=0(root)`、`Pacman v7.1.0`）。
   经由 APK + run-as + tawcroot 链路的端到端 app 启动可正常工作。

- **安装流程验证期间发现并修复的其他 bug：**
   - **`openat2` 和 `faccessat2` 经由 Android 叠加的 seccomp 过滤器杀死了
     handler 所在的宿主进程。** Android 16 的 `untrusted_app` 域对这两个
     系统调用（NR 437 和 439）RET_TRAP。两类修复：
     1. `tawcroot_path_probe_openat2()` 在 `prod_rootfs_init` 中先于
        `install_handler` 运行。没有 handler 时，Android 的 TRAP 走默认
        处置并杀死进程。调整顺序：先 install_handler，再探测（handler 现在
        会接住该陷入并路由到"无分发槽 → -ENOSYS"，探测将其解释为
        "openat2 不可用，回退到手动规范化"）。
     2. `handle_access` 和 `handle_faccessat` 在我们的 SIGSYS handler 内部
        发起 `faccessat2`。Android 的过滤器随即再次拦截，造成递归 SIGSYS，
        内核通过 force_sig 无视掩码投递，并以默认动作杀死进程。修复：
        使用 `faccessat`（NR 269）—— 较老但不受 Android 过滤器限制 ——
        并丢弃 flags（我们常见的调用方不会经 access 传入 AT_ 类 flags）。
     之前能工作的 `bash -c "exec uname"` 测试没有触发这个问题，因为
     uname 不调用 access；pacman-key 在它的第一条命令里就会调用，这也是
     它直到真实安装验证时才暴露的原因。
   - **JVM 派生 shell 链中继承的信号掩码：**
     `prod_rootfs_init` 路径（经 ProcessBuilder 从 `/system/bin/sh`
     运行）没有解除对 SIGSYS 的阻塞，而 `--exec-child` 已经做了这一步。
     实际掩码是 0x80000000（第 31 位 = 信号 32，不是 SIGSYS），所以这
     其实不是真正的阻碍 —— 但现在无论如何都会在每个入口点防御性地解除
     阻塞。

- **模拟器验证期间发现并修复的 bug：**
   - `#!` shebang 脚本：手动 loader 完全没有处理它们
     （把 `#!/usr/bin/bash` 文件当成 ELF，以 -EINVAL 失败）。
     在 `loader_exec.c` 中新增了 `resolve_shebangs`，仿照 Linux
     `binfmt_script.c`：最多读取 4 层 shebang 间接，把 argv 改写为
     `[interp, [shebang_arg,] script_path,
     orig_argv[1..]]`，并打开解释器作为实际要加载的 ELF。
     隐蔽的坑：不要让 `path_buf` 和 `argv_out[0]` 共享存储 —— 覆盖其中
     一个会悄无声息地改写另一个，bash 会看到 `argv[1] == /usr/bin/bash`
     而不是预期的脚本路径。没有这个修复，Arch 中每个以 shell 脚本为入口
     的程序（pacman-key、gpg 等）都会以 loader 的 exit 61 失败。
   - `getresuid` / `getresgid` 不在 fake-root 拦截集合中：bash 通过
     `getresuid(2)` 而非 `getuid(2)` 读取 `$UID` / `$EUID`。
     不拦截时内核返回真实的 app uid（10222），像 pacman-key 这样以
     `[[ $EUID -eq 0 ]]` 作为前提的脚本会拒绝执行。在 `identity.c` 中
     新增 handler，通过 `tawc_copy_to_guest` 向三个输出指针都写入 0。
   - 带非 NULL 空字符串的 `AT_EMPTY_PATH`：glibc 的 `fstat()` 以一个真实
     但为空的指针调用 `fstatat(fd, "", &st, AT_EMPTY_PATH)`。handler 只对
     `gpath == 0`（NULL）短路，于是继续把 `""` 相对内核 cwd 转换，把
     `fstat` 路由到了错误的 inode。wc、gpg-agent 等随后读到过时的 stat
     数据并段错误。在 `handle_newfstatat` 和 `handle_statx` 中都加入了
     通过 EFAULT 安全的 usercopy 辅助函数进行的单字节窥探。测试见
     `rootfs_smoke.c::fstatat(fd, "", AT_EMPTY_PATH)`。

   正是这些修复解释了为什么静态二进制在阶段 5 早期就能工作，而所有基于
   脚本的东西（pacman-key）以及所有使用 `bash $UID` 的东西，在这些修复
   落地前一直是坏的。

- **阶段 5b 期间发现并修复的 bug（aarch64 OnePlus 9，Android
  14，内核 5.4.284）：**
   - **aarch64 分发表中缺少 `execve`（NR 221）。** `syscalls_exec.c` 中
     之前的注释声称"aarch64 has no execve(2); glibc routes everything
     through execveat(2) there" —— 这是错的。aarch64 同时有 NR 221（execve）
     和 NR 281（execveat）。glibc 的 `execve()` 包装走 NR 221，只有
     `fexecve()` 和 `execveat()` 使用 NR 281。没有 NR 221 的 handler 时，
     `bash -c '/bin/true'`（以及其他任何普通的 fork+execve）发出的 NR 221
     未被拦截：我们的过滤器放行，Android 叠加的过滤器以 SIGSYS 杀死进程，
     guest 一尝试 exec 子进程就以 "Bad system call" 死掉。修复：在 aarch64
     上也为 `TAWC_SYS_execve = 221` 注册 `handle_execve`（包装函数
     `do_exec` 在两种架构上行为一致）。在 x86_64 模拟器上的症状：无 ——
     Android 的过滤器在那里要么放行 execve，要么不施加同样的限制，所以
     这个 bug 直到第一次真机运行前都是静默的。
   - **aarch64 上 `clone3`（NR 435）未被拦截。** glibc 2.34+ 先尝试
     clone3，在 -ENOSYS 时回退到 clone(NR 220)。没有 handler 时，我们的
     过滤器放行 clone3，由 Android 的过滤器拦截 —— 视策略版本不同，结果
     要么是静默的 ENOSYS（好 —— 回退生效），要么是被杀死。新增返回
     -ENOSYS 的 `handle_clone3`，使回退路径确定，并消除一个行为未知的面。
     `syscalls_control.c::handle_clone3` 中的注释记录了过滤器优先级的
     推理：这个修复只有在 Android 对 clone3 RET_TRAP（或 RET_ERRNO）而
     不是 RET_KILL 时才有效。经验上，clone3 由我们的 handler 返回 -ENOSYS
     后，Android 14 上的 bash fork 就能工作了。
   - **`loader_exec_child` 把 `probe_openat2` 排在
     `install_handler` 之前**，这发生在 execveat 之后重新进入的路径上，
     与已在 `prod_rootfs_init`（main.c）中修复的阶段 4 模拟器 bug 如出
     一辙。在 Android 14 上它并不是实际的阻碍（Android 14 的过滤器不拦截
     openat2 —— 正如 x86_64 模拟器实践中也从未触发过），但顺序颠倒仍然
     是 bug：`prod_rootfs_init` → 先 install_handler 再探测；
     `loader_exec_child` 中的重新初始化应保持一致。修复只有一行；调整为
     先 install_handler 再 probe_openat2。

   这些修复把"每次 bash fork 都在 6 次 sigaction 之后死掉"变成了在
   OnePlus 9 上干净地执行 `uname -a`、`id`、`pacman --version`、shell
   管道（`ls /etc | head`），全部经由 bash → fork → execve → handler →
   exec_handler_perform → execveat 自身 → --exec-child → loader 链路。

- **在 aarch64 设备上调试 pacman-key/gpg-agent 时发现的更多阶段 5b bug：**
   - **AF_UNIX bind/connect 没有转换 `sun_path`。** `bind(fd, &sockaddr_un, len)`
     中的路径位于用户态结构体内部，而不是作为内核通过 *at 类 API 解析的
     独立系统调用参数。不做转换时，gpg-agent 的
     `bind(/root/.gnupg/S.gpg-agent)` 会在宿主文件系统上找 `/root/`，得到
     -ENOENT，以状态 2 退出，导致所有 pacman-key 流程失败。修复：拦截
     `bind`（aarch64 NR 200 / x86_64 49）和 `connect`（NR 203 / 42），从
     guest 拷贝 sockaddr_un，通过 `tawcroot_path_translate` 转换 sun_path，
     并在 handler 栈上以改写后的 `/proc/self/fd/<base_fd>/<suffix>` 形式
     转发。代码位于 `src/syscalls_socket.c`。
     已确认：`gpg-agent --daemon` 现在以 0 退出，keyring socket 创建在
     宿主文件系统的正确位置。
   - **\*at handler 中的相对 fd 路径解析。** 每个 *at handler（openat、
     fstatat、statx、readlinkat、faccessat、mkdirat、unlinkat、fchmodat、
     symlinkat、linkat、renameat、renameat2）都把 guest 的 `dirfd` 传给
     `fetch_and_translate`，而后者忽略它，改为相对内核 CWD 解析相对路径。
     gpg 先打开 homedir，再发出 `openat(homedir_fd, "pubring.gpg", ...)`
     —— 经由 cwd 转换 "pubring.gpg" 得到 ENOENT，尽管对该文件 `ls` 和
     `stat` 都正常。同样的形态会让 `find -delete` 在遍历中途失败，也会
     影响所有使用 `*fd_dir(); openat(fd, …)` 模式的工具（大多数现代文件
     系统遍历都是如此）。修复：新增 `fetch_and_translate_at(dirfd, …)`
     变体 —— 当 dirfd ≠ AT_FDCWD 且路径为相对路径时，透传给内核做相对 fd
     解析。该 dirfd 本身是我们之前从一次经过转换的 openat 返回的，所以其
     inode 已经在 rootfs 视图内。注意：从 dirfd 出发的 `..` 链仍可能逃逸
     （内核会自由地越过 dirfd 走 `..`）；在没有 RESOLVE_BENEATH 的内核上，
     proot 也有同样的缺口。
   - **每个字符串的环境变量缓冲区太小。** `do_exec` 把 argv/envp 收集到
     静态缓冲区中，每个字符串 `MAX_STR = 4096`。bash 的 `LS_COLORS`、
     导出的函数体以及一些 /etc/profile.d 中的追加很容易超过 4 KB；单个
     溢出的 env 项会让 `tawc_copy_string_from_guest` 返回
     -ENAMETOOLONG，`do_exec` 将其作为 execve(2) 的返回值传播 —— bash
     对每个外部命令都打印 "File name too long"。把 MAX_STR 提高到 16 KB，
     envp_strings 缓冲区提高到 256 KB。

   bind/dirfd/env 缓冲区这三项修复到位后，最初的 pacman-key/gpg-agent
   复现用例（针对全新 homedir 的 `gpg --quick-gen-key`）现在在 aarch64
   设备和 x86_64 模拟器上都能端到端通过：pubring.kbx 被创建，主密钥生成，
   自签名写入，pacman-key --init 运行完成。早先的诊断（"gpg never tries
   `O_CREAT` on pubring.kbx"）是错的 —— 表面上缺失的 `O_CREAT` 只是更早
   失败的 socket/dirfd 路径的下游症状，而在重新测试复现用例时这些已经
   理清。问题已关闭。

   pacman-key --populate 之前失败是因为一个不相关的 `wc` 段错误（它的
   `secret_keys_available` 辅助函数把 gpg 的 --with-colons 输出通过
   管道交给 `wc -l`）。根因：`loader_exec.c` 中的 loader 为 guest 分配了
   256 KiB 的匿名栈。wc 9.11 的 `wc_lines`/`wc_bytes` 以
   `-fstack-clash-protection` 编译，在栈帧中预分配 256 KiB 的 I/O 缓冲区，
   然后向下逐页探测（`orq $0,(%rsp)`）—— 结果越过我们区域的底部一页，
   以 SEGV_MAPERR 在恰好 `rsp` 处 SIGSEGV。修复：把 loader 的栈提高到
   8 MiB，与 Linux 默认的 RLIMIT_STACK 一致；匿名页仍按需清零，所以代价是
   地址预留，而不是 RSS。

   在 aarch64 工作落地期间，x86_64 也已偏离到无法构建的状态：
   `handle_rename_legacy` 在 `do_renameat` 期望 dirfd 整数的位置传入了
   路径指针（这是在引入显式 dirfd 的 renameat 重构之前遗留的），而且
   x86_64 的 sysnr 块中缺少 `TAWC_SYS_execve`（NR 59）。没有这些修复，
   x86_64 交叉构建从未产出包含 bind/connect 转换器的二进制，因此模拟器
   运行的是一个过时的 .so，其中 bind() 完全绕过了 tawcroot，gpg-agent
   在宿主侧路径上崩溃。两个修复事后看都很小且显而易见；在此指出，是为了
   避免将来 bisect 时被误导。

   调试期间还修复了一个潜在的 stdout 与 stderr 混用 bug：
   `tawc_io_str` 把跟踪和错误输出写到 fd 1（stdout），但 tawcroot 应该
   让 stdout 保持原样留给 guest。写到 stdout 的输出会被 shell 的
   `$(...)` 命令替换捕获，变成下游命令的 argv；在 pacman-key 中复现得
   十分滑稽，将来任何跟踪输出都会悄无声息地破坏参数解析。现在写到 fd 2
   （stderr），与 main.c 中初始化处使用的 `tawcroot: …` 错误消息约定一致。

- **在 OnePlus 9（Android 14，内核 5.4）上搭建 gtk4 输入集成测试时
  发现的 bug：**
   - **`apply_memo` 在目标变短时的移位循环别名 bug。**
     常见符号链接 memo 器的原地移位，无论改写后变长还是变短，都从右向左
     写入剩余尾部。对于变短（`m->target_len < m->src_len`），最后一次
     移位迭代读取的位置已被第一次迭代覆盖 —— 路径在 `target_len` 处被
     截断。与生产相关的情形是 Arch 上的 `usr/sbin -> bin`（相对）：
     转换 `/usr/sbin/bash` 得到 `bin` 而不是 `bin/bash`，第二轮 memo
     又把它展开回 `/usr/bin`（一个目录），于是 `lstat /usr/sbin/bash`
     返回 `/bin` 处的符号链接，对于 glibc 经由 /usr/sbin 解析的任何
     二进制，`bash` 内建的 PATH 查找都会失败。修复：变短时从左向右拷贝，
     变长时从右向左拷贝。`tawcroot/src/path.c`。
   - **`faccessat`（aarch64 NR 269 / x86_64 48）未被拦截。** 只有
     `faccessat2`（NR 439）和旧式 x86_64 `access`（NR 21）有分发表项。
     glibc 的 `access(2)` 包装在所有内核上都发 NR 269，只是机会性地探测
     NR 439（5.8+，通过类 dlopen 的回退），所以在 OnePlus 9（5.4）上，
     libc 对 guest 路径的每次 access 检查都直接进入内核，内核相对宿主
     文件系统解析并返回 -ENOENT。
     症状：fontconfig 找不到 `/etc/fonts/fonts.conf`
     （`Cannot load default config file: No such file: (null)`），
     gcc 的 `find_a_file` 找不到 `cc1`，回退到裸的
     `posix_spawnp("cc1")`，随后以
     `posix_spawnp: No such file or directory` 失败。修复：在
     `TAWC_SYS_faccessat2` 之外，也为 `TAWC_SYS_faccessat` 注册
     `handle_faccessat`。该 handler 在内部的
     `TAWC_RAW(faccessat, ...)` 调用中本就丢弃 flags（我们在内部发 NR 269
     以避开 Android 对 faccessat2 的 RET_TRAP），所以同一实现可以服务两个
     系统调用号。

   这两个修复之后，当时名为 `test_input_dispatch` 的套件在
   `--es method tawcroot` 下于 OnePlus 9 上约 22 秒内通过了全部 13 个
   输入分发场景。
   这是第一个完全在设备上的 tawcroot 下运行的集成测试套件 —— 当时的
   debug app 在 chroot 安装中构建（faccessat 修复后 gcc-on-tawcroot 没有
   问题；如有需要，测试框架支持用 `TAWC_BUILD_INSTALL_ID` 在相邻的安装中
   构建），并从 tawcroot rootfs 运行，通过共享的
   `/data/data/me.phie.tawc/wayland-0` socket 对接 app 内的合成器。

- **已修复：GNU `wc` 9.11 段错误。** 一份 core dump（以 root 执行
  `ulimit -c unlimited` 后获得）显示 `si_code=SEGV_MAPERR`，fault 地址
  ≡ rsp，rip 位于一个 `orq $0,(%rsp); cmp %r11,%rsp; jne loop`
  逐页探测循环内 —— 即 GCC 的 `-fstack-clash-protection` 插桩。
  `wc_lines`/`wc_bytes` 在栈帧中预留 256 KiB 的 I/O 缓冲区
  （偏移 `-0x4008c(%rbp)`），并一次一页向下遍历。loader 的栈 mmap
  恰好是 256 KiB，所以探测循环越过了底部并 SIGSEGV。wc 在 coreutils 中
  是唯一出问题的，因为没有其他工具碰巧分配了 ≥ loader 栈大小的栈帧。
  修复：把 `loader_exec.c` 中的 `STACK_SZ` 提高到
  8 MiB（与 Linux 默认的 RLIMIT_STACK 一致）。

- **阶段 4 — 模拟器集成**：✓ APK 接入已落地；✓
   x86_64 模拟器端到端验证已完成。真实的 Arch glibc 二进制
   （uname、id、pacman --version、带管道的 bash）在 Android 16 / 内核 6.6
   模拟器上，从 app 上下文（run-as，真实的 `untrusted_app` zygote 过滤器
   生效）经由完整的 tawcroot 栈运行。包括由 SIGSYS handler 驱动的重新
   exec 流程：`bash -c "exec uname"` 和 `bash -c "ls /etc | head"` 都能
   端到端运行，依次经过 fork → 被拦截的 execve → handler →
   exec_handler_perform → execveat 进入自身 → `--exec-child` 重新初始化
   → 手动加载 → ld.so → glibc 初始化 → guest main → 退出。

   除宿主验证之外，设备上还需要一个修复：从 bash 子进程继承的信号掩码中
   SIGSYS 被阻塞（第 30 位，外加第 31 位 —— bash 正常的 exec 前信号
   规范设置），且该掩码跨 `execveat` 保留。在这个内核上，Linux seccomp 的
   `force_sig_seccomp` 不会绕过线程掩码，因此 SIGSYS 保持 pending，在我们
   新安装的 handler 本应触发时，内核以默认动作杀死进程。修复：
   `tawcroot_loader_exec_child` 在 install_handler 之后、手动加载跳转之前，
   立即通过 `rt_sigprocmask(SIG_UNBLOCK, {SIGSYS})` 强制解除对 SIGSYS 的
   阻塞（`tawcroot/src/loader_exec.c`）。`syscalls_control.c` 中运行时的
   sigprocmask 影子已经会从 guest 发起的任何掩码修改中去掉 SIGSYS，所以
   这只影响重新 exec 之后进程中继承的初始掩码。
   `tawcroot/build.sh` 为 aarch64 + x86_64 交叉构建，并把
   `libtawcroot.so` 放到 `app/src/main/jniLibs/<abi>/`；APK 像
   `libproot.so` 一样打包它。`TawcrootMethod.kt` 仿照
   `ProotMethod.kt`（rootless、rootfs 归 app-uid 所有、相同的 bind 集合、
   相同的纯 Kotlin tar 解压器），但去掉了 proot 专用的变通：
   没有单独的 loader stub，没有 `/dev/shm` 宿主 bind（在 handler 内用
   memfd 模拟），没有 `--link2symlink`（内置于 linkat handler），没有
   `MOZ_DISABLE_*_SANDBOX` 环境变量（没有 ptrace tracer 与 Firefox 的
   沙箱冲突）。安装 activity 有第三个单选按钮；
   `scripts/rootfs-run.sh` 在 chroot/proot 之外也分派 `tawcroot`。
   遗留：真机运行（阶段 5），`pacman -Syu` 运行完成（阶段 6）。
- **阶段 5 — 模拟器端到端**：✓ 已在 x86_64 上完成。通过 APK 安装成功
   （`state: READY`，`method: tawcroot`）。从宿主 shell 执行 `rootfs-run`
   可运行 `uname -a`、`id`、`pacman --version`、bash 管道
   （`ls /etc | head`）、bash fork+exec、真实 Arch glibc 二进制的手动
   加载 —— 全部正常。

- **阶段 5b — aarch64 移植**：✓ 已在 OnePlus 9（Android 14，内核
   5.4.284）上按冒烟命令标准完成。通过 APK 安装达到
   `state: READY`（`--es method tawcroot --es id arch-tawcroot`），
   且 `rootfs-run "uname -a; id; pacman --version | head -1;
   ls /etc | head"` 经由 bash → fork → execve → handler →
   exec_handler_perform → execveat 自身 → --exec-child → loader 链路
   输出干净的 Arch glibc 结果。在 bind/connect/dirfd/env 缓冲区修复之后，
   `pacman-key --init` 和 `gpg --quick-gen-key` 在 aarch64 设备和
   x86_64 模拟器上都能干净运行。
   `pacman-key --populate` 之前被一个 `wc` 段错误阻塞（loader 栈太小，
   放不下 wc 带栈冲突探测的 256 KiB 栈帧）；已通过把 loader 栈提高到
   8 MiB 修复。完整运行 `pacman -Syu` 至结束属于阶段 6 的性能工作。

   这次运行中发现并修复了三个仅 aarch64 的 bug；详见上文"阶段 5b 期间
   发现并修复的 bug"。最重要的一项：为 NR 221 注册 `handle_execve` ——
   aarch64 同时有 `execve`（221）和 `execveat`（281），之前的代码只拦截
   281，所以任何普通的 `execve()`（bash 的 `exec` 内建命令和大多数
   fork+exec 路径都用它）都绕过了我们的过滤器，被 Android 叠加的过滤器
   以 SIGSYS 杀死。

   随后的 gtk4 集成测试搭建又暴露了两个与 aarch64 相关的 bug（apply_memo
   移位别名以及未拦截的 `faccessat` NR 269）；见上文"在 OnePlus 9 上搭建
   gtk4 输入集成测试时发现的 bug"。修复之后，当时名为
   `test_input_dispatch` 的集成套件（13 个通过 Wayland 经合成器驱动
   gtk4 的输入分发场景）完全在 OnePlus 9 上的 tawcroot 下运行。

- **阶段 5c — 完整集成测试套件，OnePlus 9**（2026-05-02）：
   pacman 软件包安装以及更广的 chroot-test 测试面上线。**12 个集成测试
   全部通过**，在 OnePlus 9 上经由 tawcroot 运行，且不需要
   `MOZ_DISABLE_*_SANDBOX` 变通环境变量。落地的 Firefox 相关修复：
   handler 内的 `/dev/shm` memfd 模拟（`tawcroot/src/shm.c`），使
   Mozilla 的 `shm_open(3)` 不会硬断言；拦截 guest 的 `seccomp(2)` /
   `prctl(PR_SET_SECCOMP)`（当时是拒绝；现在是假装接受，见 status.md 中的
   偏差），使 Mozilla 无法叠加一个会绕过 tawcroot 转换不变量的过滤器；
   旧式 x86_64 `readlink(2)` 的 /proc/self/exe 合成（`readlinkat` handler
   已有，但 NR 89 没有）；宿主 auxv 透传，使合成的 guest 栈带有 HWCAP /
   HWCAP2 / SYSINFO_EHDR / CLKTCK / FLAGS；test_firefox 的稳态 AHB 断言
   从脆弱的 `wlegl: imported` 日志 grep（只在 WebRender 缓冲环仍在增长
   时成立）改写为合成器状态检查
   （`surfaces_wlegl >= 1 && surfaces_shm == 0 && frames > before`），
   它能捕获同样的回归，又不会在缓冲环稳定后误报失败。发现并修复了三个
   与 aarch64 相关的 bug：
     1. **经由 gpgme 的 close 循环死循环。** glibc 的
        `closefrom()`（gpgme 在 `fork()` 与 `execve()` 之间为 fd 清理而
        调用）通过 `getdents64` 循环枚举 `/proc/self/fd`，逐个关闭 fd，
        直到列表为空。早先版本的 close handler 对任何 ≥
        `TAWCROOT_RESERVED_FD_BASE` 的 fd 返回 `-EBADF`，但内核侧的 fd
        从未真正关闭 —— `/proc/self/fd` 一直显示 1000-1009，glibc 的循环
        永不终止，pacman/gpg-agent 在每个 scriptlet 上以 100% CPU 挂起
        数十分钟。分两部分修复：
          a. **BPF close 快速路径。** seccomp 过滤器现在对 `close(fd)`
             特殊处理：只有当 `fd` 匹配安装时烘焙进过滤器的某个实际保留槽
             （rootfs_fd + 每个 bind 的 src_fd）时才 `RET_TRAP`，否则
             `RET_ALLOW`。每个 gpgme 子进程省去约 100 万次不必要的 SIGSYS
             往返。保留 fd 列表是新的
             `tawcroot_reserved_fds[]`（在 `fdtab.h` /
             `syscalls_fd.c` 中），由 `tawcroot_fd_reserve()` 填充。
          b. **handler 真正关闭。** 对保留 fd，`handle_close` 现在调用真实的
             `close()`（不再用 `-EBADF` 撒谎）。让 glibc 的 closefrom 循环
             得以终止。guest 的 fork 子进程失去保留 fd 没有问题，因为子进程
             即将 `execve`，而我们的 exec_handler 会在 `--exec-child` 中
             重建它们。
             *（后来与 (c) 一起被撤回：`handle_close` 再次假装成功，fd
             得以保留。使之安全的是 `getdents64` 过滤器，它在
             `/proc/self/fd` 中隐藏保留 fd，使 closefrom 循环看到空列表。
             见 `src/syscalls_fd.c`。）*
        另外在 `path.c` 中还有第三部分：
          c. **保留 fd 的惰性重新打开。** `path_translate` 在每次转换开头
             通过一次 `fcntl(F_GETFD)` 系统调用探测校验
             `tawcroot_rootfs_fd` 和每个 `tawcroot_binds[i].src_fd`，
             在 `-EBADF` 时从暂存的宿主路径
             （`tawcroot_rootfs_host_path`，新增的
             `tawcroot_binds[i].src[256]` 字段）重新打开。
             每次转换多一个系统调用；这是必需的，否则父进程中 closefrom
             之后的路径系统调用会出错。
     2. **AT_EXECFN 在 guest exec 之间粘连在最初的 argv 上。**
        `tawcroot_exec_handler_perform` 把父进程暂存的
        `tawcroot_guest_exe_path` 通过重新 exec 的 memfd 转发给子进程的
        `loader_exec_child`，这意味着对每个 fork-and-exec 出的后代，
        `/proc/self/exe` 都解析为*第一个* guest 二进制（通常是
        `/bin/bash`）。Firefox 的 stub 二进制（用 `/proc/self/exe` 找到
        自己的安装目录，再相对它 `dlopen` `libxul.so`）看到的是
        `/bin/bash`，找不到 libxul，打印
        "Couldn't load XPCOM." 后退出。修复：exec handler 传入
        `extras.guest_exe = NULL`，使 `--exec-child` 回退到 `st.path`
        （实际的 exec 目标）。还需要第二轮修复（2026-05-04 发现回归）：
        `st.path` 是 guest 传给 `execve` 的原样路径，而不是解析符号链接后
        的真实路径。通过 `/usr/sbin/firefox`（→ `/usr/bin/firefox` →
        `/usr/lib/firefox/firefox`）启动的 Firefox 把 `$ORIGIN` 置为
        `/usr/sbin`，再次破坏了 libxul.so 的 dlopen。
        `tawcroot_set_guest_exe_path` 现在以 `PATH_FOLLOW` 把路径经过
        `tawcroot_path_translate`，使 `/proc/self/exe` 返回规范的
        guest 绝对路径，与真实 chroot 下内核给出的结果一致。
     3. **没有记录 bind 源的宿主路径。** bind 表之前只存 `src_fd`，在
        exec handler 时通过 `readlinkat /proc/self/fd/<src_fd>` 恢复宿主
        路径。一旦 gpgme 的 closefrom 开始真正关闭这些 fd（按修复 #1），
        readlink 就会失败，`--exec-child` 无法重建 bind 表。给
        `struct tawcroot_bind` 新增 `src[256]` 字段，在
        `tawcroot_path_add_bind` 时填充；exec_handler 现在直接从这里拷贝。

   这些修复之后，pacman 能干净地安装软件包（`pacman -S` 在 1.5 秒内完成
   pkgconf，完整的 test-deps 集合约 5 分钟），gtk3/gtk4 demo、weston、
   Vulkan 客户端、supertuxkart，以及当时名为 `test_input_dispatch` 的
   流程都在 OnePlus 9 上通过。在 wc 段错误的 loader 栈修复之后，keyring
   初始化（`pacman-key --init && --populate archlinux`）也能端到端完成。

- **阶段 6 — 加固 + 性能**：叠加过滤器的各种怪异情况，测量与调优。
