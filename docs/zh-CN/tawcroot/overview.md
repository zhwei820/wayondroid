# tawcroot —— 概述与范围

## 语言：C，cleat 仅用于测试

纯 C（C11），用 NDK clang 编译 arm64-v8a 和 x86_64。生产环境的
tawcroot 二进制**没有任何第三方依赖**。它除内核 ABI 外不链接任何东西：
手写的裸系统调用桩、手写的 io 辅助函数（`tawc_io_*`），没有 libc，
没有 cleat，没有 STC。

[cleat](https://codeberg.org/sphi/cleat) —— Sophie 的 C 库，内置一个
单元测试框架，并封装了 [STC](https://github.com/stclib/STC) 的泛型
容器 —— **只被宿主侧的测试编排器使用**（`build/tawcroot-host/tests`）。
它是一个 hosted glibc 二进制，以子进程方式 fork 出 tawcroot 二进制；
cleat / STC 代码从不在 `libtawcroot.so` 或 `tawcroot-testhost` 内部运行。

cleat 位于 `./deps/cleat/`，克隆到固定的 commit 并被 `.gitignore`
（与 `deps/libhybris/`、`deps/libxkbcommon/`、`deps/proot/` 相同的模式）。
构建脚本在 `--abi=host` 首次运行时克隆它；无需单独打包。交叉构建
（aarch64/x86_64）从不引用 cleat。

**生产代码中的容器：** 如果/当生产代码需要容器（bind 表、BPF 生成器的
程序缓冲区，……），使用手写的定长 C 数组 / 手工编写的结构。这些约束
（`-static -nostdlib -no-pie -ffreestanding`，无 malloc，无 glibc）意味着
STC 本来就用不上。bind 表在初始化时从一个扁平 C 数组构建一次；handler
直接遍历该数组。任何需要按需增长分配的东西都应放在 handler 之外、生产
代码之外：抽出一个纯函数，在 cleat 下测试它，再把结果编码成运行时里的
扁平 C 数组。

为什么是 C，而不是 C++ 或 Rust：
- **Rust** 最不适合热路径。SIGSYS handler 必须是 async-signal-safe 的
  （不分配、不展开栈、不在共享状态上触发 Drop 实现）。三者之中 Rust 的
  热路径可用子集最窄 —— panic 会展开进 Drop，整个 handler 最终都是
  `unsafe`。我们也几乎得不到 Rust 的内存安全价值，因为我们本来就信任
  guest。构建开销（为多一个 `.so` 引入 cargo + cargo-ndk）对这样一个
  自成一体的东西来说是实实在在的摩擦。
- **C++** 能在 handler 之外提供更好用的工具（RAII、模板、`string_view`），
  但会增加工具链负担（libc++ 静态链接，`-fno-exceptions -fno-rtti
  -fno-unwind-tables`，提防隐藏的分配 / 析构函数越过 handler 边界），
  回报却不多。生产 tawcroot 本来就没有 libc；C++ 能帮上忙的部分（测试
  脚手架）反正也在 cleat 编排器进程里。
- **纯 C** 是最简单可行的方案。C 是我们所接触部分（信号 handler、汇编桩、
  内核 ABI、ELF 解析、BPF）的通用语言，它能干净地配合 `-nostdlib` 链接，
  而 C 不擅长的东西（托管字符串、正则、泛型容器）我们*在生产代码内部*
  其实并不需要 —— bind 表是扁平数组，路径是定长缓冲区，BPF 程序是静态的
  `struct sock_filter[]`。在*确实*需要这些便利的地方（测试准备、抽出后的
  路径翻译单元测试、BPF 生成器校验），它们都在 `libtawcroot.so` 之外的
  cleat 测试运行器里。

设计中固化的约束：
- **生产二进制（`libtawcroot.so` / 宿主 `tawcroot`）：** 没有 libc、没有
  malloc、没有 cleat、没有 STC、没有 stdio，完全没有第三方依赖。
  `-static -nostdlib -nostartfiles -no-pie -ffreestanding`。所有系统调用
  都经过 `arch/<arch>_stub.S` 中的裸桩；所有格式化输出都经过
  `tawc_io_*`。seccomp 过滤器 + handler 结构依赖于此 —— 见
  §"为什么用 non-PIE"。
- **特别是在 SIGSYS handler 中：** 不分配、不用 stdio、不做任何会持锁的
  事，除非审计过生成的代码，否则完全不调用 libc。路径缓冲区是 handler
  栈上的定长数组（`char buf[PATH_MAX]`）。bind 表是初始化时构建的扁平
  C 数组。
- **Testhost 二进制（`tawcroot-testhost`）：** 与生产相同的约束 —— 同一个
  `_start`、同一个裸系统调用桩、同一个过滤器、同一个 handler。唯一区别是
  `-DTAWCROOT_TESTHOST` 把 `main.c` 中的 argv 分发路由到
  `tawcroot_testhost_main()`（定义在 `tawcroot/tests/testhost/src/` 下）。
  这里同样不链接 cleat / STC；冒烟驱动使用同样的 `tawc_io_*` 辅助函数。
- **Cleat 测试运行器（`tests`）：** hosted glibc 二进制，完全使用 cleat。
  以子进程方式 fork 出 tawcroot 二进制；从不与它们共享地址空间。这是本
  项目中 cleat / STC 代码运行的*唯一*地方。
- 编译选项 `-O2 -fno-strict-aliasing -fvisibility=hidden
  -fno-stack-protector -static -no-pie`。彻底 strip。handler/运行时对象中
  不得有全局构造函数、sanitizer、profiling 插桩或隐式运行时钩子。静态
  链接是强制的 —— 原因见 §"构建集成" 和 "Known gaps" #1。
  **非 PIE 是强制的** —— 迫使这一点的过滤器叠加约束见 §"为什么用 non-PIE"。
  在静态非 PIE 链接下，`-Wl,-z,now` 和 PLT 相关选项没有意义（不存在
  PLT），但 `-fvisibility=hidden` 仍有助于链接器丢弃未使用的符号。

如果产生「语言」决策的约束不再成立，这一决策可以重新评审 —— 例如我们
迫切需要真正的 ADT 来表示系统调用结果、迫切到愿意为运行时付出代价，或者
无 libc / 无分配规则不再是 handler 所需。代码库规模本身不是触发条件；它
只会单向增长。「C handler 核心」决策则更难重新评审 —— handler 的形态就是
如此。

## 为什么不继续用 proot

有几个原因，都指向同一个方向：

- **性能，无处不在。** `notes/proot.md` 记录了 proot 在系统调用密集型
  负载上约 5–10× 原生的开销。systrap 把每个被拦截的系统调用从「tracee
  停下、tracer 唤醒、两次 `ptrace` 系统调用、上下文切换回去」压缩为
  「内核在线程内投递 SIGSYS、handler 返回」—— 通常为 1.5–3× 原生。体现在
  shell 更跟手、`pacman` 更快、构建更快、系统调用密集的应用路径延迟更低
  （Firefox 启动、字体扫描、GTK 主题加载）。这不是针对单一负载的优化。
- **更少的兼容性 hack。** 进程内系统调用模拟绕开了我们目前需要变通或
  只能忍受的若干 proot 角落：独立的 loader 解包二进制，以及 proot bug
  跟踪器中记录的 tracer/tracee fork 竞争。仍然需要一些兼容行为 —— 特别是
  `/proc/self/exe` 合成、hardlink-as-symlink 兜底和 fake-root uid/stat
  行为 —— 但这些都变成本地的系统调用 handler，而不是 ptrace 时代的机制。
  handler 运行在 tracee 线程内，能完整访问其保存的寄存器，因此 proot 的
  许多偶然复杂性根本不需要。
- **自己掌控的代码库。** proot 是一个 vendored 的 Termux fork，其上游并不
  很适合 Android；它的代码质量参差不齐，架构由我们并不共有的 ptrace 约束
  塑造。一个围绕 systrap 模型构建的小而地道的 C 实现更容易推理、调试和
  扩展（例如我们以后想要更好的 `/proc` 合成、可选的沙箱或 binder-fd
  重写）。
- **热路径上没有意外的性能劣化。** ptrace 会拦截 tracer 要求查看的*每个*
  系统调用；即使用 seccomp 预过滤，每次陷入也有固定开销。RET_TRAP 只对
  我们真正关心的约 30 个系统调用触发，而对其余约 320 个的 BPF 求值低于
  一微秒。不存在「因为教了 proot 一个新系统调用所以它变慢了」这种情况。

`pacman -Syu` 是最直观的基准 —— 在模拟器上全新安装的 ALARM 上目前约需
7 分钟，几乎全部受系统调用限制 —— 所以它是我们测量时会引用的主要数字。
但收益是普遍的；pacman 只是最明显的地方。

## 为扩展而设计

当前范围是替代 proot 的最小集：路径翻译、fake-root uid/stat 行为、
hardlink-as-symlink 兜底、`/proc/self/exe` 合成、`getcwd` 反向翻译、ELF
loader 注入。成熟的 proot 在此之外所做的大部分事情（fake-`mknod`、mount
namespace 垫片、更精细的 `/proc` 合成 —— 包括 `/proc/<pid>/maps` 反向翻译，
因为手动加载后的映射在那里显示宿主侧 rootfs 路径 —— 完整的 `io_uring`
SQE 重写，也许将来还有 binder-fd 重写），随着我们让更多负载跑在 tawcroot
上，我们很可能也会需要。按需添加，不要提前做。

`chroot(2)` 已实现（见 §"chroot 模拟"）。该模拟把
`tawcroot_rootfs_fd` + `tawcroot_rootfs_host_path` + 带 `active` 标志的
bind 条目作为「当前根视图」；chroot 原地替换所有这些并重建符号链接 memo
缓存。handler 位于一个 .c 文件（`src/chroot.c`）加一个纯辅助函数
（`path_orchestrate.c` 中的 `tawcroot_path_binds_reanchor`）。

因此架构被刻意组织成让以后添加这些东西变得廉价，而不只是为了快速交付
MVP：

- **按子系统划分的 handler 文件**（`src/syscalls_fs.c`、
  `syscalls_fd.c`、`syscalls_exec.c`、`syscalls_socket.c`、
  `syscalls_control.c`，以及用于较大功能的 `chroot.c` / `shm.c` /
  `linkstore.c`），这样新功能就是「加一个 `.c` 和一个分发条目」，而不是
  「编辑一个巨型文件」。每个文件以一个填充其分发槽位的
  `tawcroot_*_register()` 结尾。
- **分发表是按系统调用号索引的定长数组**，未处理的为 `NULL`。添加一个
  系统调用就是填一个槽位并把它加入 BPF 允许列表 —— 没有顺序隐患。
- **BPF 过滤器在运行时由分发表生成**（在 `prod_rootfs_init` 中，通过
  `tawcroot_dispatch_trap_list`）。分发表 —— 由各子系统的
  `..._register()` 调用填充 —— 是 handler 路由和 BPF 允许列表共享的唯一
  事实来源。
- **路径翻译、ELF 解析、bind 表查找和 fake-root 元数据装饰与 handler
  分发相互独立**，因此新策略（例如更完整的 xattr 或设备节点模拟）作为
  分发条目上的又一层装饰加入，而不是重写。
- **下面的「Open questions」一节是一份活的列表。** 其中的条目（沙箱、
  `io_uring`、USER_NOTIF 回退）在需求出现时被提升为实际工作。

「现在要不要加这个」的标准是「我们交付的负载需要它吗？」设计决策的标准是
「这会让将来需要的东西变得更难吗？」不要固化会排除未来扩展的假设。

## 它明确不是什么

- **不是沙箱。** 没有安全边界。与 proot 一样，重写器信任 guest。过滤器
  是路由机制，而不是隔离机制。真正的隔离来自 Android 已有的
  `untrusted_app` SELinux 域（它本来就包裹着整个应用进程）。
- **不是 Linux ABI 模拟器。** 与 gVisor 的 Sentry 不同，宿主内核仍然实现
  几乎所有系统调用。我们对接受 rootfs 相对路径的系统调用重写参数，合成
  TAWC 需要的那一小块 fake-root 身份面，并修正少数被 Android 叠加过滤器
  拒绝的系统调用。其余一切直接透传。
- **不是完整的用户态 namespace 替代品。** 我们确实伪造了 TAWC 原本依赖
  proot `-0` 提供的那一小块 root 身份面：`getuid`/`geteuid`/`getgid`/
  `getegid` 返回 0，`SO_PEERCRED` 把同 uid 的 unix socket 对端报告为 root
  （这样 tmux/dbus 这类基于 peer-cred 认证的服务端会接受 guest 客户端；
  `SCM_CREDENTIALS` 被刻意不重写 —— 见 `syscalls_socket.c` 中的 handler
  注释），`stat` 族结果在适当时被装饰为 root 所有，改变属主的系统调用被
  当作兼容性操作而不是真正的特权变更（使用 `--owners` 时，stat 和 chown
  改用持久化的属主表，见 docs/rootless.md "属主存储"）。我们
  **不**提供 pid namespace、mount namespace、user namespace 语义，或通用
  的特权模型。

  **CAP_DAC_OVERRIDE 被惰性模拟。** 内核在每个真实系统调用上看到的都是
  应用 uid，因此拒绝*属主*的权限模式也会拒绝 guest 的「root」——
  `chmod 555 /` 之后 `mkdir /x` 曾返回 EACCES。proot 的 `-0` 的解决办法是
  在每个路径系统调用之前把每个路径分量 chmod 放宽；我们只在错误路径上做
  同样的事。当 handler 在虚拟 euid 0 下返回 EACCES 时，分发包装器沿翻译器
  记录的路线放宽应用拥有的 inode，重跑一次 handler，再恢复旧的模式 ——
  因此 guest 仍然看到它设置的模式（`pacman -Qkk`、sudo 的 0440 sudoers
  检查、ssh StrictModes 都保持干净）。规则和限制见
  docs/tawcroot/path-translation.md §"DAC override"；实践中会碰到的一条是
  没有属主*搜索*位的目录，它在路径翻译中、路线存在之前就会失败。

  **身份在两个方向上都只是表面的，这对多用户 guest 很重要。** `set*id`
  写入一个影子结构（`identity.c`）；内核仍看到应用 uid，因此降为 guest
  「用户」并不会放弃任何实际权限 —— 该进程仍然可以读写 rootfs 中的每个
  文件，包括其他用户的 home 和 `/etc/shadow`。`SO_PEERCRED` 对内核 uid
  是我们的*每个*对端都报告 0，因此 guest 内基于 peer-cred 的 socket 认证会
  把所有人都认证为 root。而且影子结构位于 guest 共享的进程内存中，可以被
  直接改写。结论：**一个 rootfs 就是一个单一的安全主体。** guest 侧的
  用户账户是兼容性功能，不是边界。任何把 shell 交给你不愿交出整个 rootfs
  的人的东西 —— sshd、`su` 到服务账户、多用户配置 —— 都是把整个 rootfs
  交给了他们（如果启用了 `ando`，还包括应用的 Android 权限）。凡是记录
  此类负载的地方都要说明这一点；不要把 guest uid 描述为隔离。
- **不是 32 位模拟器。** Android lp32 正在被淘汰，我们的发行版只有 lp64。
  作为纵深防御，seccomp 过滤器会对任何 32 位 personality 系统调用执行
  `KILL_PROCESS`。
- **不是 tracer 进程。** 我们考虑过 `SECCOMP_USER_NOTIF`（内核通过 fd 把
  陷入投递给独立的监督进程）并否决了它 —— 那会让我们回到 proot 的
  tracer/tracee 模型，失去「单进程、直接内存访问、无需编组」的优势。如果
  信号安全出了问题，答案是「修信号安全 bug」，而不是「换架构」。

所以 libhybris / GPU / binder 的情况不变。`/dev/kgsl-3d0` 等上的 ioctl
被 `RET_ALLOW` 并直接到达宿主内核，这正是我们想要的。
