# tawcroot — 已知缺口、行为偏差、后续工作与维护

## 已确定的基础

MVP 之前的设计必须先定下来的问题。它们都已确定并实现；条目仍然保留，
是因为其中的参考资料（loader/auxv 规则、过滤器安装约束、resolver 模式
列表）在维护相关代码时仍然用得上。**这是一份记录，不是待办清单** ——
进行中的工作在 `issues/` 和 `plans/` 里。

1. **~~`libtawcroot.so` 必须静态链接~~ — 已解决。**
   已确认可以针对 bionic 的 `libc.a` 静态链接。NDK r27 为所有架构提供
   `libc.a`。`-static -no-pie` 产出的是 ET_EXEC 二进制，没有 `PT_INTERP`，
   没有 `DT_NEEDED` —— 内核直接加载它，不经过 bionic linker。在我们的
   `_start` 之前不会有任何拦截。

   静态链接同时也是**基于 IP 的 seccomp 过滤器在多次 re-exec 之间仍然
   有效的必要条件** —— 见 §"为什么用 non-PIE"。这是比最初动机（避开 bionic
   linker）更强的约束。

   实现要点：
   - `seccomp()` 和 `execveat()` **没有 bionic libc 封装**。使用原始的
     `syscall(__NR_seccomp, ...)` 和 `syscall(__NR_execveat, ...)`。
     系统调用号和常量（`SECCOMP_SET_MODE_FILTER`、`SECCOMP_RET_TRAP`、
     `struct sock_fprog` 等）可从 `<linux/seccomp.h>` 和
     `<linux/filter.h>` 获得。
   - `sigaction`、`mmap`、`mprotect`、`memcpy`、字符串函数在静态 bionic
     中都可用。
   - `errno` 基于 TLS，在静态 bionic 中工作正常。
   - `pthread` 从 API 23+ 起可用（21-22 缺 `pthread_atfork`；无关紧要，
     我们的目标是 API 28+）。
   - 静态 bionic 二进制**只能在 Android 上运行** —— 由于 bionic 特有的
     内核初始化预期，它们在桌面 Linux 上会段错误。宿主侧测试必须使用
     宿主工具链，而不是 NDK。
   - 二进制大小：简单程序 strip 后约 450 KB；完整的 tawcroot 估计
     1–2 MB。

2. **~~`--exec-child` 的进程内程序加载器~~ — 宿主上已解决。**
   手写 loader（解析器、PT_LOAD 映射器（含 BSS 部分页清零和匿名扩展）、
   带完整 auxv 和 `AT_RANDOM` 的栈合成、各架构 trampoline）已经实现，
   能把静态和动态 guest 一路运行到退出。
   `tawcroot/tests/integration/test_prod_exec` 覆盖 `/bin/true`，
   `tawcroot/tests/unit/test_loader_smoke_dynamic` 覆盖 `/bin/ls`，
   `tawcroot/tests/integration/test_exec_child` 和
   `test_exec_via_handler` 完整往返验证 memfd 交接。argv、envp 和 auxv
   都完整保留（在 `dynamic_argv_check.c` 中验证）。

   尚未完成的部分（需要在 Android 上、在路径转换下验证）：
   - 针对 fake rootfs 运行同一个 loader（依赖阶段 2d）。
   - 在 aarch64 设备上、在真实的 `untrusted_app` zygote 过滤器下运行
     （依赖阶段 4 的 APK 接入）。
   - 在真实设备上验证 `MAP_FIXED_NOREPLACE` 相对于 tawcroot 高位非 PIE
     基址（aarch64 为 `0x2000000000`，x86_64 为 `0x40000000`）的放置 ——
     宿主 glibc 测试对地址空间约束的考验方式不同。

   下面的参考资料对今后的维护仍然有效（BSS 规则、AT_RANDOM、内核 auxv
   顺序）；"tawcroot 风险最高的部分"这一定位已经撤销。

   - **ELF 头读取器**（约 50 行）：读取 `Elf64_Ehdr`，校验 magic，提取
     `e_type`/`e_entry`/`e_phoff`/`e_phnum`/`e_phentsize`。
   - **PT_LOAD 段映射器**（约 100 行）：遍历 program header；对每个
     `PT_LOAD`，计算页对齐地址，用 `MAP_FIXED_NOREPLACE` mmap 文件支撑
     的部分，处理 BSS。对 **PIE（ET_DYN）** guest 二进制：先做一次大的
     `mmap(NULL, total_span, PROT_NONE, MAP_ANON, ...)` 预留地址范围，
     再在预留区内对每个段做 MAP_FIXED（基址由内核选择，免费获得 ASLR）。
     对 **ET_EXEC**（非 PIE）guest 二进制：直接用 `p_vaddr` 配合
     `MAP_FIXED_NOREPLACE`。
   - **BSS 部分页清零** —— 棘手的部分。段的最后一个文件支撑页可能同时
     包含已初始化数据和 BSS。必须从文件 mmap 整页，然后 `memset` 把
     `p_vaddr + p_filesz` 到页边界之间的字节清零。如果段是只读的，这一页
     需要临时设为可写。额外的完整 BSS 页（`p_memsz > p_filesz` 超出文件
     支撑范围的部分）以匿名方式映射（内核预先清零）。遗漏部分页清零是
     手写 loader 的常见 bug；linker 通常会用零填充，但并不保证。
   - **栈合成器**（约 150 行）。与 proot 的 loader（复用内核构建的栈并
     原地修补 auxv）不同，我们必须构建全新的栈，因为 guest 没有经过内核
     exec。按 SysV ABI 布局，自底向上：`argc`、argv 指针、NULL、envp
     指针、NULL、auxv 条目，其上是字符串数据。复制我们自己的 auxv，并
     覆盖与程序相关的字段：
     - `AT_PHDR`（二进制基址 + `e_phoff`）
     - `AT_PHENT`、`AT_PHNUM`
     - `AT_BASE`（ld.so 加载地址；静态二进制为 0）
     - `AT_ENTRY`（二进制的 `e_entry`）
     - `AT_EXECFN`（guest 请求的路径 —— 必须是新栈上的字符串，指针随之
       更新）
     - `AT_UID`、`AT_GID`、`AT_EUID`、`AT_EGID`（全部设为 0，以匹配
       fake-root `-0` 的系统调用行为；不要在这里继承 Android 应用 uid）
     - **`AT_RANDOM`**（**关键** —— 必须指向新栈上 16 个新鲜的随机字节。
       glibc 启动时读取这些字节来初始化栈 canary 用的
       `__stack_chk_guard`。没有有效的 `AT_RANDOM`，程序会立即
       `abort()`。用 `getrandom(2)` 填充。）
     - `AT_SECURE`（设为 0）
     - `AT_PLATFORM`（把*字符串*复制到新栈并更新指针 —— 继承来的指针
       指向我们即将丢弃的旧栈）

     从我们自己的 auxv 原样继承：`AT_PAGESZ`、`AT_CLKTCK`、
     `AT_HWCAP`/`AT_HWCAP2`、`AT_SYSINFO_EHDR`（vDSO 基址 —— vDSO 映射在
     mmap 新段之后依然存在，可以安全继承；没有它，`clock_gettime` 会
     回退到真实系统调用，性能下降可测）。初始栈位于一块新 `mmap` 的区域
     （8 MiB，与内核默认 RLIMIT_STACK 一致，低端加 `PROT_NONE` 保护页），
     而不是我们现有的栈上。
     **入口处 `SP` 必须 16 字节对齐** —— 在 aarch64 上是硬件要求
     （未对齐的 SP 会 fault），在 x86_64 上是 ABI 要求（SSE 依赖它）。
   - **TLS**：完全由 ld.so 处理，不归 loader 管。ld.so 读取已加载二进制
     及 DT_NEEDED 库的 `PT_TLS` 头，分配 Static TLS Block，并设置线程
     指针寄存器。我们只需要正确的 `AT_PHDR`/`AT_PHNUM`，让 ld.so 能找到
     这些头。
   - **brk / 进程堆**：手动加载不会像 `execve` 那样重置内核的 program
     break。guest glibc 可能在 ld.so 启动后用 `brk` 做早期 malloc。需要
     有意识地选择：要么预留一块新的 guest 堆区域并在跳转前用原始
     `brk(2)` 设置内核 break，要么给它一个合理但隔离的 break 值，迫使
     guest 的分配走 `mmap` 路径。guest 跳转之后，tawcroot 运行时不得使用
     malloc 或任何假定拥有旧 bionic 堆的 libc 路径。在扩大系统调用覆盖面
     之前，加一个运行动态链接程序、执行大量小 malloc 的 smoke 测试。
   - **各架构入口 trampoline**（每个架构约 15 行汇编：把 `rsp`/`sp` 设到
     合成栈顶，清零 `rdx`/`x0`（rtld_fini），跳到 ld.so 的入口 —— 静态
     ELF 则跳到二进制的 `e_entry`）。确切的寄存器约定见
     `deps/proot/src/loader/assembly-{arm64,x86_64}.h`。
   - **地址空间布局约束**（约 50 行）。libtawcroot.so 是位于高位固定
     地址的非 PIE（见 §"为什么用 non-PIE"）；guest 二进制和 ld.so 的地址由
     我们在其下方选定。在 `--exec-child` 启动时读取 `/proc/self/maps`。
     在 tawcroot 自身映射周围预留 `PROT_NONE` 保护区（约 256 MB），使
     内核不会把 ld.so 之后的 `mmap(NULL, …)` 分配（用于 DT_NEEDED 库）
     扩展到我们的 text/data 中。所有显式放置都使用
     `MAP_FIXED_NOREPLACE` —— 重叠时它干净地报错（`-EEXIST`），而不是
     静默覆盖。
     `MAP_FIXED_NOREPLACE` 在所有相关的 Android 内核上都可用（4.17
     引入；Android 11+ 最低是 5.4）。

   **实现策略：参照资料自行编写。**
   我们不 vendor proot 的 loader（GPLv2 —— 会给 tawcroot 带来许可证
   负担），也不整体搬运 musl（它的 loader 只解决了一半问题；见下文）。
   而是在 tawcroot 自己的许可证下全新编写一组 `src/loader_*.c`，并以
   下列资料为参照来比对我们的行为：

   - **musl `ldso/dynlink.c::map_library`**（MIT）—— PT_LOAD 映射计算的
     权威参考：addr_min/addr_max 遍历、ET_DYN 预留 + `MAP_FIXED` 流程、
     BSS 部分页清零、`p_memsz > p_filesz` 时的匿名扩展。当我们的 PT_LOAD
     映射器与实际不符时重读这个文件，但要基于我们的 fdtab/`tawc_raw_*`
     API 重写，而不是逐字复制。musl **不**合成栈 —— 它修补内核构建的
     栈 —— 所以 auxv 和栈布局部分在 musl 中没有对应物。
   - **proot `src/loader/loader.c`**（GPLv2，仅作参考）—— 证明进程内
     映射 + 跳转模式在 Android 上可行。可作为运行时参照（在 proot 下运行
     一个二进制，dump 其初始栈，与我们的比对），但不复制任何代码。
   - **Linux 内核 `fs/binfmt_elf.c::create_elf_tables`** —— 关于用户态
     在初始栈上看到什么的权威规范。auxv 列表、顺序以及
     `AT_RANDOM`/`AT_PLATFORM` 的字符串复制语义都来自这里。内核头文件和
     ABI 本身不是受版权保护的接口；行为才是。
   - **glibc `csu/libc-start.c`** + glibc 的
     `elf/dl-sysdep.c::_dl_sysdep_start` —— guest 程序*期望*看到的东西。
     调试"二进制启动后立即 abort/段错误"时有用 —— 通常是缺了或错了某个
     auxv 条目。

   预计约 500–800 行，分布在 `src/loader_elf.c`（解析器 + phdr 几何，
   约 200 行）、`src/loader_map.c`（PT_LOAD 映射器 + 地址空间约束，约
   250 行）、`src/loader_stack.c`（初始栈合成 + auxv，约 250 行）和
   `src/arch/<arch>_loader_jump.S`（每个架构约 15 行）。

   **测试先行的分步推进。** 每一部分在下一部分基于它构建之前都附带
   cleat 测试：
   1. 解析器/几何：合成的 ELF 缓冲区 + 真实 `/bin/true` 的头，逐字节
      断言
   2. 映射器：在宿主上映射真实 ELF，遍历 `/proc/self/maps`，断言 prot
      位 + BSS 字节为零
   3. 栈：构建一个栈，以内核的视角遍历它，逐条与 `getauxval` 输出比对
   4. trampoline + 静态二进制 smoke：加载并运行新构建的
      `static-hello`，断言退出码 + 捕获的 stdout
   5. 动态 smoke：动态链接的 `/bin/true`，然后在 fake rootfs 内运行
      `/bin/sh -c "ls /"`（阶段 2 的退出关口）

   第 4 步通过之前，阶段 2 的其他工作都不开始；第 5 步通过之前，阶段 3
   不开始。

3. **路径规范化是主要机制，不是兜底。**
   `openat2(RESOLVE_IN_ROOT)` 需要内核 5.6；我们的主要测试设备（OnePlus
   9，Android 14）运行内核 5.4，没有它。因此 handler 中手写的、感知
   symlink 的规范化，是当前*所有*设备上*所有*带路径系统调用的主代码
   路径。（后来曾在新内核上尝试 `openat2` 快速路径，又回退了 —— 它会
   破坏跨 bind 的绝对 symlink；见 `include/path_resolve.h`。guest 调用
   `openat2` 得到 `-ENOSYS` 并回退。）§"翻译规则" 中描述的常见
   目录记忆化缓存（`/lib` → `usr/lib`、`/lib64` → `usr/lib`、`/bin` →
   `usr/bin`、`/sbin` → `usr/bin` 等）对热路径性能是必需的，不是可选的
   —— 每次程序启动时 ld.so 打开库都会走这些 symlink。

4. **~~Bionic 静态链接的注意事项~~ — 已确认可行。**
   已用 NDK r27 测试。针对 bionic 的 `libc.a` 静态链接可行。已确认：
   - 没有 `dlopen`/`dlsym` —— 我们不用。
   - 没有经 netd 路由的 DNS 解析 —— 我们不做 DNS。
   - 没有 properties（`__system_property_get`）—— 我们不需要。
   - `pthread` 在静态 bionic 中从 API 23+ 起可用。
   - `errno` 基于 TLS，工作正常。
   - `sigaction`、`mmap`、`mprotect`、`memcpy`、字符串函数都存在且
     可用。
   - 静态 bionic 二进制只能在 Android 上运行（在桌面 Linux 上段错误）。
     这不是问题 —— 设备端二进制由 NDK 交叉编译；宿主测试二进制使用宿主
     工具链。

   仍需在阶段 1 早期做 smoke 测试 —— 链接一个简单的 NDK
   `-static -no-pie` 二进制，它安装 seccomp 过滤器 + SIGSYS handler，
   验证原始系统调用的 BPF 白名单与 syscall 指令标签一致，带着一个非
   `CLOEXEC` 的 state fd exec 自身，并验证继承下来的过滤器 + 重新安装的
   handler + 状态交接都能工作。它还必须在真实的 Android 应用 zygote
   过滤器下逐个执行 handler/bootstrap 依赖的每个原始系统调用。这样在
   编写任何路径转换代码之前，就验证了整条 re-exec 链。

5. **~~内部 fd 表与保留区间~~ — 已解决。**
   `include/fdtab.h` + `src/syscalls_fd.c`：tawcroot 拥有的 fd 被 dup 到
   `TAWCROOT_RESERVED_FD_BASE` 或以上，并拦截 `close`/`close_range`/
   `dup`/`dup2`/`dup3`/`fcntl`，使 guest 对*这些特定 fd* 的操作表现为
   `-EBADF`。这个 base 只是放置位置/快速路径的下限 —— 高于它的 guest fd
   仍归 guest；`close_range` 通过遍历 `/proc/self/fd` 并跳过我们的 fd
   来模拟（绝不重新发出 NR 436，API 34 之前的 Android 会拦截它）；
   `fcntl(F_DUPFD*)` 不加保护直接放行。BPF 的 close 快速路径是对
   `args[0]` 的范围比较（有意取表的超集），因此过滤器安装之后才保留的
   fd 也被覆盖。`getdents64` 在 `/proc/<pid>/fd` 列表中隐藏保留 fd，
   正是这一点阻止了 glibc 的 `closefrom` 回退路径无限循环。
   退出条件已满足：`close_range(3, ~0U, 0)` 之后的 `open` 仍然会被转换。

6. **~~guest `SIGSYS` 与 seccomp 虚拟化~~ — 已解决。**
   `src/signal_shadow.c` 保存 guest 可见的 SIGSYS 处置（seqlock 保护，
   进程级）以及按 tid 索引的 blocked 位；`src/syscalls_control.c` 虚拟化
   `rt_sigaction(SIGSYS)`，从 `rt_sigprocmask` 掩码中剥除 SIGSYS，并
   假装接受 guest 的 seccomp 安装。真实的处置和非阻塞状态始终归我们。
   代价见下文"guest seccomp 安装被假装接受"。

7. **~~路径 resolver 模式与 fd 来源~~ — 已解决。**
   `tawcroot_path_mode`（`include/path.h`）是显式枚举 ——
   `FOLLOW` / `NOFOLLOW` / `PARENT_CREATE` / `PARENT_REMOVE` —— 与用于
   只读 bind 检查的 `tawcroot_path_intent` 配对。基于 fd 的调用通过
   `/proc/self/fd/<n>` 反向转换，而不是用来源表（真正的来源表仍是一个
   需先 profile 的性能想法，见"后续工作"）。由
   `tests/unit/test_path_resolve` 和 hosted fs-handler 测试套件覆盖。

## 已接受的系统调用保真度偏差

已知、有意与内核精确语义不同之处，收集自 2026-06 的系统调用保真度
审查。每一条都被判定为不值得付出忠实模拟所需的复杂度（或返工）；只有
当真实 guest 碰到时才重新讨论。（那次审查中与实际使用相关的发现 ——
末尾斜杠语义、保留 dirfd 的 EBADF、chown 存在性探测、getdents64 目录中途
EOF、/proc shadow 的 CLOEXEC、unlink/rmdir/linkat 的 errno 形态、
`/proc/self/cwd` 合成、跨进程 exe 替换 —— 都已修复，并由
unit/hosted/smoke 测试覆盖。）

- **`statx()` 即使在 5.8 之前的内核上也报告 `STATX_MNT_ID`**
  （有意的*额外*保真，与本节通常条目方向相反）。当 guest 请求
  `STATX_MNT_ID`/`STATX_MNT_ID_UNIQUE` 而内核两者都不返回时，handler 以
  O_PATH 打开目标，并从 `/proc/self/fdinfo/<fd>` 解析 `mnt_id:` ——
  与 statx 会返回的数字相同（syscalls_fs.c 中的
  `tawcroot_statx_fill_mnt_id`；AT_FDCWD 目标固定 cwd，因为 fdinfo 中
  没有这个哨兵值的条目）。systemd ≥260（内核基线 5.10）缺少该位时返回
  EUNATCH，并去掉了自己的 fdinfo 回退，这让 5.4 内核设备上的 Debian sid
  安装变砖（见 notes/installation.md "Debian sid: keep full systemd
  out"）。shm 的 statx 合成器也会填这个字段 —— 条目取自它们的 memfd，
  合成的 `/dev/shm` 目录取自一次性探测的 memfd（所有 memfd 共享内核
  唯一的内部 shm_mnt）—— 因此 chase() 进入 `/dev/shm` 也不会 EUNATCH。
  叶子的重新打开总是 O_NOFOLLOW，并以与调用方 statx 结果的 dev/ino 匹配
  作保护，因此与叶子替换竞争的 guest 线程既无法让查找经过 resolver 从未
  见过的 symlink，也无法把另一个对象的挂载 id 拼接到结果上 —— 这种情况
  下直接跳过填充。尽力而为：任何失败都保持内核的答案不变。≥5.8 内核上
  零开销。`MNT_ID_UNIQUE` 本身（内核 6.8）不合成 —— 没有东西需要它，
  fdinfo 中也没有唯一 id 可解析。
- **guest seccomp 安装被假装接受，并不实际应用。**
  `seccomp(SET_MODE_STRICT|FILTER)` / `prctl(PR_SET_SECCOMP)` 以内核
  完全相同的 EFAULT/EINVAL 形态校验参数，什么都不安装，然后返回成功
  （syscalls_control.c）。如实叠加行不通（guest 过滤器会杀死或短路
  handler 自己的原始系统调用 —— 我们的 text 没有 IP 豁免），而之前如实
  返回的 `-EPERM` 破坏了 stock openssh（≥ 2026 年中的 portable 提交
  `7ab700f170`，它让 preauth 子进程中 sandbox 安装失败成为致命错误）
  （Firefox 和 dropbear 能容忍拒绝；sshd 会重置每个连接）。接受的后果：
  请求约束的 guest 得不到任何约束 —— 包括 sshd 自己的 preauth privsep
  sandbox，而且是在面向网络的服务上（进程本身已经处于 tawcroot 的过滤器
  + Android 应用 sandbox 之下，且所有 guest 进程共享同一个 uid，所以它想
  要的边界在这里基本不存在 —— 见 docs/tawcroot/overview.md §"What it
  explicitly is not" 中关于一个 rootfs 就是一个安全主体的说明）；另外，
  insn 数组只检查是否可读，不经过 BPF verifier —— 畸形程序会"安装"成功，
  而内核会返回 EINVAL。`SECCOMP_FILTER_FLAG_NEW_LISTENER` 仍如实返回
  `-EPERM`（成功意味着承诺一个没有任何东西支撑的 user-notif fd）；
  `PR_GET_SECCOMP` 直接放行，如实报告 filter 模式。
- **uevent netlink 监听器是一个静默的 AF_UNIX 桩。**
  `socket(AF_NETLINK, *, NETLINK_KOBJECT_UEVENT)` 对 `untrusted_app`
  返回 EACCES，而 libudev 把 NULL 监听器视为致命错误 —— 这会杀死
  `SDL_INIT_HAPTIC`，而它（SDL3 的 `SDL_Init` 是原子的）又连带拆掉了
  guest 已初始化的 *video* 子系统，于是 SDL 游戏报告 "Video subsystem
  has not been initialized" 后退出。只有在内核已经拒绝之后，
  `handle_socket` 才替换为一个没有任何人发送数据的 AF_UNIX 数据报
  socket；libudev 后续的调用通过伪造 `sockaddr_nl` 的 `bind()`、以及从
  `getsockname()` 报告合成的 `sockaddr_nl` 来满足（syscalls_socket.c，
  `TAWC_UEVENT_TAG`）。消费者因此枚举到零个设备，也永远看不到热插拔
  事件 —— 这是如实的，因为应用 uid 确实收不到这些事件，而且这与没有
  udevd 的容器中 libudev 本来就会回退到的形态相同。该桩通过其绑定的
  abstract 名称识别自身，因此不需要维护一张跨 fork/exec 或 fd 复用都要
  保持一致的 fd 表。有意*不*推广到其他 netlink 协议：`NETLINK_ROUTE`
  在 bind 时保持真实的 `EACCES`，这样 `getifaddrs()` 的消费者仍然快速
  失败，而不是等待一个永远不会来的回复。由
  `test_prodenv_uevent_socket_stub`（唯一运行在 untrusted_app 域中的
  套件，在那里拒绝是真实的）外加一个 hosted 契约测试覆盖。
- **只读 bind 的 errno 形态**（完整表格见 docs/tawcroot/
  path-translation.md §"只读 bind"）：*源*在只读 bind 中的 linkat
  即使在同一文件系统上也返回 `EXDEV` —— 宿主上的 link 会成功，并造出
  一个指向只读内容、rootfs 名下可写的硬链接，所以我们有意拒绝（工具会
  降级为复制）；移入/移出只读 bind 的 renameat 统一返回 `EROFS`，而内核
  跨挂载的版本会是 `EXDEV`；只读 bind 下的 `open(missing, O_CREAT)` 即使
  *父目录*不存在也报告 `EROFS`（内核：`ENOENT`）。
- **`execveat(AT_SYMLINK_NOFOLLOW)` → `-ENOSYS`。** syscalls_exec.c 中
  如实的占位。我们运行的东西都不使用这个 flag（`fexecve(3)` 用的是
  AT_EMPTY_PATH），而 -ENOSYS 比静默跟随 symlink 更安全。如果将来出现
  这样的调用方：按 NOFOLLOW 转换，对叶子做 `fstatat`，是 symlink 则
  返回 -ELOOP，否则按跟随的情况继续。
- **fork 之后 signal-shadow 的陈旧状态。** fork 出的子进程继承父进程
  按 tid 索引的 `blocked` 表（COW）；子进程内的内核 tid 复用可能读到一个
  陈旧的"SIGSYS blocked"位，直到该线程第一次调用 `rt_sigprocmask`。
  影响上限是一次错误的 shadow 掩码读取；修复它需要在 fork/clone 返回
  路径上重置这张表，而这个竞争从未被观察到。
- **过小的 `sigaltstack` 会被替换。** 小于 `TAWC_SIGALT_MIN` 的 guest
  altstack 会被替换为 tawcroot 拥有的 16 KiB 槽位（sigsys-handler.md
  "Handler 栈预算"），因此 guest *自己的* handler 也在那里运行 ——
  处置是按信号的，但栈是按线程的。读回是如实的；一个把自身 SP 与自己的
  缓冲区比对的 handler 会察觉到。槽位在以下情况下泄漏：线程非自愿死亡；
  fork 子进程中其他线程的槽位；在 `SS_AUTODISARM` handler 内调用的
  `sigaltstack`。vfork 子进程替换继承来的被替换栈时会释放父进程的槽位；
  位于被替换槽位上的 `SS_AUTODISARM` handler 两次替换其 altstack 时
  同样如此。
  所有这些都受兜底约束：slab 耗尽 → 原样使用 guest 的栈。
- **已释放但仍注册的 altstack 会被写入。** `SA_ONSTACK` 意味着每次被
  拦截的系统调用都会写 guest 的 altstack，而不仅是它自己的少数信号，
  因此未先 `SS_DISABLE` 就释放缓冲区的 guest 会损坏这块内存。这是
  guest 的 bug；Go、Rust std 和 Python 的 faulthandler 都会先禁用。
- **路径 scratch 池理论上可能活锁。** 一条 handler 调用链在获取更多槽位
  时持有 128 个槽位中的 3–5 个，而获取在耗尽时会无限自旋 —— 几十个线程
  同时处于调用链中途就可能卡死池。在实际的 guest 线程数下这只是理论
  问题；`TAWCROOT_SCRATCH_DEBUG_SPIN_TRAP` 构建会在需要调试时拦截这种
  自旋。
- **symlink 分量之后的 `..` 按词法解析。** fold 在 resolver 能判断前一个
  分量是否为 symlink 之前就折叠了 `..`，因此 `/a/sym/../x`（sym →
  /b/c）命中 `/a/x`，而内核命中 `/b/x`（已在宿主构建上演示）。这是
  结构性取舍，不是疏忽：fold 之后的后缀不含 `..`，这正是 rootfs 逃逸
  防护易于审计的原因。与内核一致的 `..` 意味着 resolver 必须在遍历中途
  处理它，并逐步重新检查包含关系 —— 这是 fold/resolver 的返工，不是
  补丁。（相关的末尾斜杠丢失问题**已经**修复，因为它只涉及最后一个
  分量；见 path_orchestrate.c 中的 `has_trailing_dir_marker`。）
- **深路径上限为 256 个分量**（fold 返回 -ENAMETOOLONG）；内核在 4096
  字节限制内可接受约 2040 个单字节分量。没有真实的目录布局接近 256。
- **`/proc` magic link 被限制在视图内，这在两个方向上偏离内核。**
  *通过*链接解析 —— `root`、`cwd`，以及任何其他进程的 `fd`/`map_files`
  —— 到视图之外的目标会返回 `-ENOENT`，而内核会交出宿主的 inode；
  我们自己的 `root` 链接解析到 *guest 的*根（真实 chroot 的答案），而
  不是内核会给从未 chroot 的进程的宿主根。两者都是有意的：规则、自身 fd
  的例外和遗留问题见 docs/tawcroot/path-translation.md §"`/proc`
  magic-link containment"。这是上面 `..` fold 的包含性论证终于变得
  足够，而不仅仅是成立。
- **跨进程的 `/proc/<pid>/*` 不做反向转换。** 只有调用进程自己的 /proc
  视图会得到 shadow/合成处理；其他 guest 进程的 maps/cwd/exe/fd 原样
  显示宿主路径（指*读取*这些链接 —— 通过它们解析是受限的，见上一条）。
  同 uid 的进程本来就能整体读取彼此的 /proc，所以这不会泄漏内核本来
  不会泄漏的东西。包括被模拟 chroot 的 guest 的 `/proc/<pid>/root`，
  外部观察者看到的是 "/"，而真实内核会显示 chroot 目录。
  跨进程的 `comm`/`cmdline`/`environ` 自 proctitle 工作以来**是**与内核
  一致的（proctitle.h：在 loader 跳转时执行 PR_SET_NAME + 原地重写参数
  区；该区域通过 exec_state 的 proctitle 作为 execveat 的 argv[0] 按字节
  精确预留大小，execveat 的 envp 指向 state 中的 guest 环境字符串）——
  正是这一点让 pgrep/pkill/ps 能识别 guest 进程。调用方的 argv[0] 也原样
  传递（登录 shell 的 "-sh"、busybox 分派、`exec -a`）；早先的 loader
  版本会用 exec 路径替换它。`/proc/<pid>/exe` 在跨进程时仍然是
  libtawcroot.so：PR_SET_MM_EXE_FILE 需要 CAP_SYS_RESOURCE，而且
  /proc/self/exe 必须继续指向我们的二进制 —— exec handler 通过它
  re-exec（本进程的读取会被合成为 guest 路径）。`pgrep -x`/`pgrep`/
  `pkill` 匹配的是 comm/cmdline 而不是 exe，因此只有跨进程 readlink exe
  的工具才会看到宿主路径。
- **无法写入 `security.capability` xattr。** SELinux 拒绝
  `untrusted_app` 的 CAP_SETFCAP，所以设置这个 xattr 返回 EPERM；在
  pacman 安装期间，libarchive 将其降级为逐文件警告（`newuidmap`、
  `gst-ptp-helper`、……），文件落地时不带 capability 位。运行 `setcap`
  的 scriptlet 也以同样方式通过 `cap_set_proc` 失败（不同路径，同一个
  根本拒绝），pacman 报告 `error: command failed to execute correctly`。
  **不要**让 `setxattr` 谎报成功：这个位确实不在磁盘上，exec 时的
  capability 授予确实不会发生，而这条警告是用户唯一准确的信号。我们也
  无法伪造 —— 没有 exec 时应用 capability 的层来支撑这个谎言。实际上这
  无关紧要：在 fake-root 下（没有 user namespace）`newuidmap`/
  `newgidmap` 无关，`gst-ptp-helper` 是可选的小众插件。真正需要文件
  capability 的工作负载必须使用仅用于调试的 `chroot` 安装方式（su 可以
  设置该 xattr），或者被声明为在 tawcroot 下不支持。
- **分发表中缺失的 x86_64 专有遗留系统调用落到 `-ENOSYS`。** bionic
  从不发出它们，所以 Android 的 seccomp 白名单对它们 RET_TRAP；x86_64
  glibc/musl/原始调用方会发出。拦截集合已针对真实模拟器 zygote 过滤器
  做过实证审计（prod-env `test_prodenv_legacy_nr_trapset_audit`，来自
  `dynamic_legacy_nr_probe`）—— 这是权威方法，因为内核缺少
  `CONFIG_CHECKPOINT_RESTORE`，无法 dump 过滤器。
  已处理的转发重定向，每个都指向其在白名单中的现代对应调用：
  `getpgrp`→getpgid(0)、`time`→clock_gettime、`alarm`→setitimer
  （syscalls_control.c）；`select`→pselect6、`pipe`→pipe2、
  `eventfd`→eventfd2、`signalfd`→signalfd4、`epoll_create`→epoll_create1、
  `inotify_init`→inotify_init1（syscalls_fd.c）；以及更早的
  `poll`→ppoll、`dup2`→dup3、`epoll_wait`→epoll_pwait、`getdents`
  重新打包，和走 *at 路由的路径系列。只有 `pause`（34）是已知未处理的
  —— 原始 pause(2) 会得到 -ENOSYS，但 glibc 把 pause/alarm 走
  setitimer，所以没有真实调用方会碰到；如果出现了再加 handler。仅限
  模拟器；aarch64 从未分配这些编号。如果在 x86_64 模拟器上出现奇怪
  行为，prod-env 探测是首先要扩展的地方；修复模式是一个小的转发
  handler + 一个 hosted 测试 + 一行探测表。

## 后续工作

推迟的想法；都没有排期。项目策略：除非正确性需要，或有非常大的、经
profile 证实的收益，否则不增加按内核版本区分的备选代码路径。

- **io_uring 完整拦截。** 目前 `io_uring_setup`/`enter`/`register` 以
  `-ENOSYS` 拒绝（syscalls_control.c），使 guest 回退到普通系统调用 ——
  放行 io_uring 会让带路径的 SQE（`OPENAT`、`STATX`、……）绕过路径转换。
  如果真有工作负载需要 io_uring 的吞吐量：拦截 setup/register/enter 以及
  ring 的 mmap；拒绝 `IORING_SETUP_SQPOLL`（`untrusted_app` 没有
  `CAP_SYS_NICE`，且拒绝它能让 `io_uring_enter` 保持为唯一关口）；在
  enter 时把带路径的 SQE 操作数改写为我们拥有的、已转换的缓冲区，并在
  对应 CQE 完成时释放；把未知 opcode 作为显式的 allow/warn/deny 决策
  处理。大约 500–1000 行，放在独立的 `src/uring.c` 中。
- **更多 `/proc` shadow。** 仅在有工作负载需要时扩展现有的 shadow 模式
  （proc_shadow.c）。一个分类器类型加一个内容合成器即可同时覆盖 open
  和元数据（stat/statx/access）接口 —— 后者是在加入
  `version`/`uptime`/`loadavg` 时添加的，因为 Android 对
  `untrusted_app` 既拒绝 `read` 也拒绝 `getattr`，所以只覆盖 `open` 的
  shadow 在 stat 时仍然 EACCES（LibreOffice 的 `oosplash` 只会 *stat*
  `/proc/version`，并以 "ERROR: /proc not mounted" 直接退出）。
  当前已 shadow：`<pid>/maps`、`sys/kernel/overflow{uid,gid}`、
  `bus/pci/devices`、`stat`、`version`、`uptime`、`loadavg`。
  仍被拒绝且未 shadow、尚无工作负载需要的：`cmdline`、`filesystems`、
  `sys/kernel/osrelease`、`kallsyms`、`modules`、`vmstat`。（`cpuinfo`、
  `meminfo`、`mounts`、`self/maps`、`self/status` 可以正常读取。）其他
  可能的候选：`/proc/<pid>/auxv`、`/proc/<pid>/task/<tid>/maps`。
  （`/proc/<pid>/cmdline` 不再需要 shadow —— 自 proctitle 工作以来内核
  值就是真实的；`/proc/stat` 以合成的 btime + 仅 idle 的 cpu 行做了
  shadow，因为 procps `ps` 硬性要求 `btime`，而 `/proc/uptime` 报告的
  idle 与 uptime 相等，使两者一致。）

已考虑并否决 —— 没有新证据不要重新提出：

- 用 **`PR_SET_SYSCALL_USER_DISPATCH`** 替代 seccomp-BPF：Linux 5.11
  在 x86 上引入它；Linux 6.1 ARM64 不支持。它同样保留 SIGSYS 投递开销，
  并且需要在 fork/clone/exec 之后重新启用。它不是当前 S24 上的优化
  手段；见 [execution-techniques.md](execution-techniques.md)。
- **在内部 exec 之间携带 bind/root/store fd**（在 `--exec-child` 中重新
  打开它们每个 bind 约 4 个系统调用）：已构建并测试（非 CLOEXEC 的保留
  锚点，exec-state v11）；10 个 bind 时宿主上对子进程的 strace 从 167 →
  129 个系统调用，但在 Galaxy S24 上做的固定、交错 A/B（3 轮 × 7 次，
  `perf/macro.sh`）显示墙钟时间没有变化（spawn ×1.05，bash 启动 ×0.96，
  其余 ±4%）。系统调用数的降低没有转化为耗时的降低。额外的 fd/状态
  面被否决。后续的分阶段测量记录在
  [exec-performance.md](exec-performance.md)。
- **紧凑的 argv/env exec-state 偏移：** 用序列起点替代固定偏移数组，
  把头部从 22,412 字节缩小到 1,940 字节，每次 spawn 减少约五次 minor
  fault。在 S24 上，匹配的 NDK 27.2 构建只把 shell spawn 中位数改善了
  0.84%，且结果取决于运行顺序；应用侧对比未能证明有用的收益。已回退；
  wire 版本仍为 10。
  见[测量结果](../../../tawcroot/perf/results.md#compact-exec-state-experiment-rejected)。
- **共享的每次 exec arena**（把 argv/env 直接收集到最终的 state 映射
  中）：S24 上 shell spawn 中位数 −1.1%，代价是约 700 行改动；已否决。见
  [测量结果](../../../tawcroot/perf/results.md#2026-10-18-shared-exec-arena-experiment-rejected)。
- **`openat2(RESOLVE_IN_ROOT)`（内核 5.6+）resolver 快速路径**：尝试后
  回退 —— 它会把跨 bind 的绝对 symlink 目标重新定根到 bind 源 dirfd
  （破坏了 `/system/lib64` → `/apex` 的 bionic）。见
  `include/path_resolve.h` 顶部的历史说明。
  `RESOLVE_NO_SYMLINKS` 探测（只拒绝，绝不重新定根）可以保持 resolver
  的语义，但应用策略并不统一：phasing.md 记录 Android 16
  `untrusted_app` 会拦截 `openat2`，而 Android 17 上的 Galaxy S24
  （`untrusted_app_34`，内核 6.1）允许它（2026-10-18 探测）。共享路径
  缓存（`include/pathcache.h`）不依赖它也覆盖了同样的开销。

## 已确认的环境

已在主要测试设备（OnePlus 9，Android 14，API 34，内核 5.4.284）上实证
验证：

- **`Seccomp:2`** —— 应用进程已启用 zygote 安装的过滤器
- **`execveat`** —— 内核中存在该系统调用（`__arm64_sys_execveat`）
- **`MAP_FIXED_NOREPLACE`** —— 可用（内核 4.17+，我们是 5.4）
- **`openat2`** —— **不可用**（需要 5.6）。回退到手写路径规范化
- **`PR_SET_SYSCALL_USER_DISPATCH`** —— **不可用**（需要 5.11）。使用
  BPF IP 检查
- **`PR_SET_NO_NEW_PRIVS`** —— 在 `untrusted_app` SELinux 域中可用，
  不需要 `CAP_SYS_ADMIN`
- **`seccomp(SECCOMP_SET_MODE_FILTER)`** —— 在 `untrusted_app` 中可用
  （seccomp 安装路径上没有 SELinux hook）
- **`apk_data_file` exec** —— 已确认可用（proot 的
  `libproot-loader.so` 已经证明了这条路径）
- **被拦截系统调用的栈开销** —— 一次需要解析路径的 `openat` 在 SP 之下
  约 6.2 KiB（约 4.5 KiB 内核帧，无 SVE，+ handler）
- **NDK 静态链接** —— NDK r27 中所有 ABI 都有 `libc.a`。
  `-static -no-pie` 产出无 PT_INTERP 的 ET_EXEC

## 维护约定

- C 代码是我们自己的。保持精简、符合惯例。**生产代码没有第三方依赖**
  —— 没有 libc，没有 cleat，没有 STC，什么都没有。cleat（及其 vendor 的
  STC）只存在于宿主侧测试编排器中；修改 `tawcroot/build.sh` 中 cleat 的
  固定版本是一项有意的改动，只影响测试。
- 不要往生产代码里加 libc、运行时或容器库。如果需要容器，就在初始化时
  从扁平的 C 数组构建结构，或者抽出一个纯辅助函数并在 cleat 下测试。
  bind 表是典型示例。
- 遵循项目已有的约定：脚本放在 `scripts/`，安装器代码放在
  `me.phie.tawc.install`，笔记放在这里。
- 新增被拦截的系统调用时，需要加到：(1) BPF 过滤器生成器，(2) 分发表，
  (3) 为任何新的纯辅助函数加 `tawcroot/tests/unit/` 测试，(4) 加一个
  `tawcroot/tests/handler/` 测试，通过 `tawcroot-testhost` 针对 fake
  rootfs 驱动该系统调用，(5) 待生产代码具备可用的 ELF 加载 + 跳转路径
  后，加一个 `tawcroot/tests/integration/` 测试。
  `tawcroot/test.sh` 运行以上全部；CI 在每次 push 时运行它。
- **不要**在 `tawcroot/src/main.c` 中添加 `--run-test`、smoke-driver 或
  其他测试用 argv 分支。仅测试用的入口代码放在
  `tawcroot/tests/testhost/src/` 下，并由 `-DTAWCROOT_TESTHOST` 控制。
  生产代码只能通过真实 CLI 到达。
- SIGSYS handler 保持 freestanding、无分配、不依赖 libc。如果某个功能
  需要在 handler 中使用容器，说明设计有误 —— 重新组织，使容器在初始化
  时构建，handler 只读取扁平的不可变视图。
- 设计变化时更新这份笔记。未来的 Sophie 会感谢你。
