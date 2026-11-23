# tawcroot — 测试策略

## 测试策略

**测试在宿主上运行。每个功能在实现的同时就要被测试。** 没有测试的改动
不算改动。

### 双二进制拆分

生产 tawcroot 不得包含测试脚手架或诊断用 argv 分支 —— 没有 smoke
driver，没有 `--run-test` 钩子，生产 CLI 上没有 loader 诊断。生产 argv
接口恰好只有两个入口：

- `tawcroot -r ROOTFS [-b SRC:DST]... -- CMD [ARGS...]` —— rootfs 模式
  的启动路径。
- `tawcroot --exec-child <fd>` —— 从 SIGSYS execve handler 流程重新进入
  （`exec_handler.c` 把 exec_state 写入一个 memfd，并以这个 argv
  re-exec `/proc/self/exe`）。不是仅测试用的 —— handler 在正常的 guest
  exec 中就会使用它，所以它在生产中必须保持可达。

仅 testhost 的诊断 flag 位于 `main.c` 中的 `#ifdef TAWCROOT_TESTHOST`
之后：

- `--exec PATH [ARGS...]` —— 直接调用手写 ELF loader，不做路径转换，
  没有 handler/过滤器。让集成测试能单独覆盖 loader vtable。
- `--exec-via-handler PATH [ARGS...]` —— 端到端驱动
  `tawcroot_exec_handler_perform()`（写 memfd → re-exec
  `/proc/self/exe` → `--exec-child`）。当 testhost 是 re-exec 目标时，
  它自己的 `--exec-child` 分派会检查 `argv[2]` 是纯整数（loader 子进程
  路径，生产语义）还是 `--state-fd=<n>`（foundation-smoke 路径）。

测试层位于两个地方：

1. **`tawcroot-testhost`** —— 一个*第二个*二进制，由相同的生产源码加上
   `tawcroot/tests/testhost/src/{testhost_main,
   smoke,child,rootfs_smoke}.c` 构建，以 `-DTAWCROOT_TESTHOST` 编译。
   相同的 `_start`、相同的原始系统调用 stub、相同的过滤器、相同的
   handler —— 唯一的区别是 `main.c` 把非 loader 的 argv 分派路由到
   `tawcroot_testhost_main()`（它位于 `tawcroot/src/` 之外），再加上
   上面的 loader 诊断 flag。smoke driver *在 testhost 内部*发出内联汇编
   系统调用，使每个调用的 IP 都在 stub 白名单之外，于是过滤器把它们
   拦截进真实的 handler。handler 层的测试不可能放在别处：IP 白名单把它们
   与被测二进制绑定在一起。testhost **永远不会**打包进 APK；它的存在
   只是为了让 cleat 编排器能 fork 它。

2. **`tests`（cleat 编排器）** —— 一个 glibc/bionic 二进制，位于
   `build/tawcroot-{host,aarch64,x86_64}/tests`，由
   `tawcroot/tests/{unit,handler,integration}/*.c` 构建并链接 cleat +
   STC。宿主构建在本地运行；交叉构建在设备上通过
   `tawcroot/test.sh --device` 运行。这是整个项目中 cleat / STC 代码
   *唯一*运行的地方。它负责：
   - 过滤器语法（针对 `module`、`name` 或 `module::name` 的全匹配正则；
     多个参数之间是 OR；见 cleat docs/test_framework.md），
   - 退出码（0 = 通过，1 = 失败），
   - 统一的报告。

   编排器不与 tawcroot 共享地址空间。对 handler 层的用例，它以相应的
   argv（如 `-r <rootfs>`）`fork`+`exec` `tawcroot-testhost`，捕获
   stdout/stderr/退出码，并对结果断言。对集成用例，它 fork 真实的生产
   `tawcroot`（待该二进制的手写 ELF loader 足以跳转到 guest 之后）。
   需要时也可以用 cleat 的 `test_capture { ... }` 做自 fork 测试。

`tawcroot/test.sh` 是 cleat 编排器的一个薄封装；`tawcroot/test.sh`
运行全部测试，位置参数成为 cleat 过滤器。`--device` 模式把 NDK 交叉
构建的编排器（以及 tawcroot、tawcroot-testhost、fixture 和
androidfilter wrap）推送到设备上标准的 scratch 目录 `$TAWC_SCRATCH`
（`/data/local/tmp/tawc-dev/`，见 `scripts/lib/tawc-scratch.sh`），然后
以 adb shell 方式 exec 它。二进制形态与宿主模式相同 —— 只是 ABI 和
`TAWCROOT_TEST_TMPDIR` 不同（`$TAWC_SCRATCH/tt-rootless` 对比 `/tmp`）。
PASSTHROUGH 过滤器原样传给编排器，退出码就是编排器自己的（通过
`__exit=$?` 哨兵捕获，因为 adb shell 并不总能干净地透传退出码）。

### cleat / STC 绝不链接进生产

这是一条硬性规则：

- `tawcroot/src/`（生产）—— 没有 cleat，没有 STC，没有 glibc，完全
  没有 libc。生产保持 `-static -nostdlib -nostartfiles
  -ffreestanding -no-pie`。
- `tawcroot/tests/testhost/src/`（仅 testhost）—— 同样的约束。smoke
  driver 使用与生产相同的 `tawc_io_*` 原始系统调用辅助函数。cleat / STC
  在这里同样不可达；这个二进制是 freestanding 的。
- `tawcroot/tests/{unit,hosted,handler,integration}/`（cleat 编排器）
  —— 可以自由使用 cleat / STC。hosted glibc 二进制。hosted 层与被测
  生产代码共享地址空间（这正是目的 —— ASan 能看到它）；handler /
  integration 层 fork 真实的 freestanding 二进制。

每个生产源文件都能在两种模式下编译（整个 `PROD_C` 集合都在
`PROD_C_FOR_TESTS` 中）；仅 freestanding 的部分恰好是 `src/arch/*.S`。
原始系统调用的 extern 在 freestanding 二进制中解析到汇编 stub，在编排器
中解析到 `tests/hosted/raw_syscall_host.c`。

### 五个测试层

每个相当于一个 PR 的工作单元都应至少在其中一层加测试，理想情况下是一个
第 1/1.5 层的逻辑测试加一个第 2 层或第 3 层的功能测试。

**Sanitizer**：宿主 `tests` 编排器总是以 ASan+UBSan 构建
（`tawcroot/Makefile` 中的 `TESTS_SANFLAGS`，
`-fno-sanitize-recover=all`，任何命中都使运行失败；`test.sh` 设置
`ASAN_OPTIONS=detect_stack_use_after_return=1:strict_string_checks=1`）。
宿主的 `tawcroot` / `tawcroot-testhost` 二进制（仅为测试产物 —— 发布的
二进制是未插桩的 NDK 交叉构建）使用 trap 模式的 UBSan
（`TAWC_UBSAN`）：没有运行时，UB 变成 SIGILL，被 fork 的测试随之失败。
整个生产 C 代码树都编译进 hosted 测试二进制：`PROD_C_FOR_TESTS = PROD_C`
加上 hosted 原始系统调用垫片 `tawcroot/tests/hosted/raw_syscall_host.c`，
它提供 `tawcroot_raw_syscall`（内联汇编，严格的 `-errno` 约定），因为
汇编 stub 带有 `_start`，无法与 glibc 链接。该垫片还暴露
`tawcroot_test_raw_hook` —— 一个可由测试安装的拦截器，用于故障注入和
系统调用观测。只有 `src/arch/*.S` 仍然仅限 freestanding。

1. **Unit（`tawcroot/tests/unit/`）** —— 纯函数测试，直接用 cleat，
   没有 fixture 状态：字符串辅助函数、路径 fold/resolve/orchestrate
   （vtable mock）、loader ELF/map/stack、BPF 程序生成、dirent 过滤、
   signal shadow、proc 重写。纯 C，不 fork。亚秒级。每次保存都运行。
1.5. **Hosted（`tawcroot/tests/hosted/`）** —— 针对真实生产 handler 的
   handler 逻辑测试，在进程内、ASan 下运行：harness（`hosted.{h,c}`）
   构建一个 tmpdir rootfs，将其打开并保留为当前根（supervisor 初始化的
   步骤 1–6，没有过滤器/SIGSYS），`th_sys()` 以合成的参数直接调用分发
   handler。"guest 内存"就是普通的测试缓冲区 —— usercopy 通过
   `process_vm_readv` 指向自身。teardown 对 `/proc/self/fd` 做 diff，
   因此 fd 泄漏会让测试失败（fd 表是产品主要的可泄漏资源；sanitizer 不
   跟踪它）。`tawcroot_test_raw_hook` 在 handler 中途注入故障
   （EINTR/ENOSPC/EMFILE），并观测 handler 实际发出的转换后系统调用。
   新的 handler 逻辑覆盖默认放在这一层；下面的 fork 层保留那些确实需要
   真实产物的部分（过滤器安装、SIGSYS 投递、stub IP 白名单、
   `_start`/bootstrap）。
2. **Handler（`tawcroot/tests/handler/`）** —— 以选定的 argv fork
   `tawcroot-testhost`，捕获 stdout，解析 `tawc_io_step` 输出的每一行
   `[ok ]` / `[FAIL]`，每个检查注册一个 cleat 测试。
   testhost 自身含有驱动 SIGSYS handler 的内联汇编探针；cleat 用例负责
   搭建 fixture（如 fake rootfs）、exec，并通过 cleat 的
   `register_dynamic_tests` 把每个检查单独呈现出来。每个解析出的通过项
   成为一个空操作的 cleat 测试；每个失败项成为一个
   `register_test_problem`，其消息包含该步骤行及其后附的 kv 上下文。
   共享的辅助代码是 `tawcroot/tests/handler/steps.{c,h}`。

   当前的模块：`handler/test_foundation_smoke`（拦截契约 + 原始系统调用
   遍历 + state-fd 交接 + `--exec-child` re-exec），
   `handler/test_rootfs_syscalls_smoke`（路径转换 + 运行时不变量）。
   它们合计产出 100 多个单独命名的 cleat 测试；每个文件一次 testhost
   exec。新增一个检查只需在 testhost 中加一行 `tawc_io_step()` 调用 ——
   cleat 侧在下次运行时自动识别。

   测试名来自 `tawc_io_step` 的标签。保持标签稳定（不嵌入运行时值，
   不用 unicode 箭头/破折号）；约定见
   `tawcroot/tests/testhost/src/smoke.c::exercise_one`（rv 放在单独的
   kv 行上，使标签保持稳定）。
3. **Integration（`tawcroot/tests/integration/`）** —— 针对宿主构建的
   fake rootfs（一个 tmpdir 目录树，含若干常见文件，以及用于 `execve`
   路径的静态 `dash`/`busybox`）完整运行 `tawcroot
   -r /tmp/fake-rootfs -- <child>`。`<child>` 程序是测试套件从
   `tawcroot/tests/integration/programs/` 构建的小型 C 二进制，各自覆盖
   特定行为："cat /etc/foo 返回 rootfs 内容"、"绝对路径的 openat 会被
   转换"、"带 `..` 的相对 openat 被限制在 rootfs 根"、"错误的路径指针
   返回 `EFAULT` 而不是崩溃"、"getuid/stat/chown 保持 fake-root 假象"、
   "guest 的 `close_range` 无法杀掉 tawcroot 的内部 fd"、
   "guest 的 `sigaction(SIGSYS)` / `sigprocmask` 无法禁用转换"、"guest
   的 seccomp 安装被假装接受而不实际安装"、"linkat 在 Android 风格的
   `EPERM` 下回退到 symlink"、"execve 动态链接的二进制会经由 PT_INTERP
   到达 loader"等。当前的模块：`test_prod_rootfs` / `test_prod_features` /
   `test_prod_fork` / `test_exec_child` / `test_exec_via_handler` /
   `test_diag_exec`。
4. **Differential（`tawcroot/tests/diff/`，未来）** —— 与第 3 层相同的
   `<child>` 程序，分别在 proot 和 tawcroot 下运行，对比 stdout/退出码。
   第 3 层存在后成本很低；信号价值高，因为除了少数我们有意偏离的地方
   （快速路径、经由 memfd 的 `/dev/shm` 等），proot 的行为就是一切的
   规范。尚未构建。
- **合成的 Android 过滤器测试**（`tawcroot/tests/handler/
  androidfilter/wrap.c` + `tawcroot/tests/handler/test_androidfilter.c`）：
  一个前置过滤器的包装程序，在 exec testhost 之前安装一个形如 Android
  `untrusted_app` 的 seccomp 过滤器（对 `openat2`/`faccessat2`/`clone3`/
  `close_range` RET_TRAP，在 x86_64 上再加 lp64 遗留集合）。通过
  `--include-legacy-x86_64` / 默认拆分复用整个 rootfs 系统调用步骤列表
  —— 产出 `androidfilter` 和（在 x86_64 上）`androidfilter_legacy`
  模块，x86_64 上约 104 个 cleat 测试。无需设备就能捕获 "Bugs found and
  fixed during install pipeline validation" 中记录的探测顺序 / handler
  递归 / 分派覆盖类 bug。包装程序**不**安装 SIGSYS handler —— 在
  tawcroot 的 handler 就绪之前，任何被拦截系统调用的默认处置都是终止
  进程，这与设备上的行为一致。有意不拦截的例外（`execve`/`execveat`，
  以便包装程序能启动子进程）见包装程序的头部注释。`close_range` 是被
  拦截的：API 34 以下的 Android 策略会拦截 NR 436，因此在这里拦截它才能
  证明 handler 是在模拟而不是重新发出（wmww/tawc#14）。
  发现了一个 bug：testhost 的 `prod_rootfs_init` 把 `probe_openat2`
  排在 `install_handler` 之前，与几个月前在生产 main.c 中修复的 bug
  如出一辙。见 `tawcroot/tests/testhost/src/rootfs_smoke.c`。
- **真实工作负载 smoke**（`tawcroot/tests/integration/workload/`）：在
  开发机上通过 tawcroot 驱动一个 `pacstrap` 出来的 Arch chroot，包括
  `pacman -Syu`。在这里把性能收益作为回归测试来度量，而不只是一个基准
  数字。

### Prod-env 设备层（broker / `untrusted_app`）

`tests/integration/tests/tawcroot_prodenv.rs`（staging/spawn 辅助代码在
`tests/integration/src/tawcroot_prodenv.rs`）**在真实生产 sandbox 中运行
生产的 `libtawcroot.so`** —— 应用 uid、`untrusted_app`、zygote 安装的
seccomp 过滤器 —— 通过 exec broker 的 ARGV 形式 exec 它，与生产启动的
方式相同。属于 `scripts/run-integration-tests.sh` 的一部分（过滤器：
`tawcroot_prodenv::`）。

没有任何测试二进制接触设备：`libtawcroot.so` 本来就在 APK 的
`nativeLibraryDir` 中（应用可以 execve），guest 程序 + fake rootfs 都是
由手写 ELF loader mmap 的纯数据。rootfs 通过 broker 的
`sh -c 'cat > …'` 逐文件暂存到应用私有缓存
（`/data/data/me.phie.tawc/cache/tawcroot-prodtest/`）；guest 程序是
来自 `build/tawcroot-<abi>/programs/` 的第 3 层 NDK 交叉构建
（run-integration-tests.sh 中的 `ensure_tawcroot_device_tests` 负责
构建）。宿主从 broker 的 `app-info` action 获知 `nativeLibraryDir`。

这一层新覆盖的内容：

- **合成 androidfilter 的真实基准**：guest 在 zygote 实际安装的过滤器下
  运行，两个目标上都是。合成版本（`tests/handler/androidfilter/wrap.c`）
  与现实之间的偏差会变成测试失败。
- **生产域中的生产 SELinux 交互**：例如硬链接被拒 → v1 rename+symlink
  回退（`static_link_publish_argv12`）、应用数据目录中的 AF_UNIX bind
  （`static_unix_bind_argv1` —— adb-shell 套件在 `shell_data_file` 上
  必须跳过这一项）。
- **没有 adbd 模式差异**：broker 子进程从来不是真正的 root，因此
  rooted-adbd euid-0 跳过这一类问题（见下文"设备环境敏感点"）在这里
  不会再出现。
  `static_drop_ids_devnull_eperm` 是 dropped-identity EPERM/EACCES
  smoke 步骤在 guest 可观测层面的移植版本。

**测试放置规则：凡是能在 prod-env 中完整测试的，就在 prod-env 中测试。**
不要在 prod-env 和 adb-shell cleat 设备套件之间重复同一个检查，除非重复
确实增加了覆盖（并在重复所在处说明原因）。具体来说：

- guest 程序能观测到的、与设备相关的新检查（路径转换结果、errno 表现、
  身份假象、linkat/O_TMPFILE 行为）放进 prod-env guest 程序，而不是
  testhost smoke 步骤。
- 按构造只能留在 testhost 的：拦截契约、过滤器安装、stub IP 白名单、
  `--exec-child` re-exec 管线、loader 诊断 —— 任何必须*作为*
  supervisor 本身、或需要发出过滤器之前的原始系统调用的东西。
  `tawcroot/test.sh --device` 为这些保持原样（包括其已记录的环境
  敏感点）。

### 需要 Android 端到端验证的内容（小清单）

不属于常规测试循环 —— 这些在接入时手动运行一次，之后定期作为 smoke
运行：

1. APK 接入（`TawcrootMethod`、jniLib 打包、包装脚本生成、
   `rootfs-run` 中的分派）。
2. 从 `nativeLibraryDir` execve 的 SELinux smoke —— 现在已是常规覆盖：
   每次 prod-env 测试运行（见上文）正是这件事。
3. 在真实部署中与 proot 的最终性能对比。
4. 在真实设备上检查 libhybris/AHB 的系统调用覆盖。

这就是全部仅限 Android 的测试面。其余一切都在开发机上。

### Android 上的 smoke 检查

上面四个仅限 Android 的项接入现有的 `scripts/run-integration-tests.sh`
harness，它已经支持 `TAWC_TARGET=physical|emulator`，并通过
`RootfsRunner` 抽象安装方式。为现有的 proot/chroot 集成测试添加
tawcroot 变体 —— 它们应当是纯粹的分派新增，而不是新的测试逻辑。模拟器
覆盖合成 Android 过滤器下 x86_64 上 lp64 `access` 的情况，设备覆盖
libhybris/AHB 的系统调用覆盖。

### 设备环境敏感点（踩坑得来）

- **Rooted adbd（rooted 模拟器变体）**：testhost 以真实 uid 0 运行，
  所以"丢弃身份 → 真实 EPERM/EACCES"这些 smoke 步骤无法成立 —— 丢弃
  只是虚拟的，真实 root 可以随意 chmod/chown root 拥有的 `/dev/null`。
  smoke 在过滤器安装前捕获真实 euid，并在真实 root 下对这些步骤
  `tawc_io_skip`。
- **Rooted adbd，第二部分**：惰性 DAC-override 测试
  （`tests/hosted/test_dac_override.c`，以及 smoke 的
  `test_dac_override_on_unwritable_root`）需要 DAC 真正生效。在真实
  uid 0 下内核从不拒绝，所以 hosted 用例只在 `geteuid() != 0` 时注册，
  smoke 步骤则 `tawc_io_skip`。
  `dac_not_owned_route_is_not_rescued` 还会探测一个测试 uid 不拥有、且
  `mkdir` 恰好返回 EACCES 的目录 —— 在 Android 上 `/` 是只读 rootfs
  （EROFS），根本到不了救援逻辑。
- **Unrooted shell（物理设备、rootless 模拟器）**：宿主 `link(2)` 被
  SELinux 拒绝，所以在宿主/rooted 模拟器运行中原样通过的 linkat 接口在
  这里真正触发模拟 —— 它们曾经是唯一端到端覆盖 v1 rename+symlink 回退的
  常规环境（现在 prod-env 层在两个目标上有意覆盖它）。已经踩过的三个
  后果：`test_linkat_happy_path` 必须以感知 v1 的方式恢复 `/etc/probe`
  （v1 会在旧名称处留下一个反向 symlink；unlink 新名称会让它悬空，而
  smoke fixture 被之后的每个步骤共享，在 `test_androidfilter.c` 中还被
  之后的套件变体共享 —— 现在这些变体也会在两次运行之间重建 fixture）；
  `test_legacy_x86_64_wrappers` 中 x86_64 遗留的 `link`/`rename` 块需要
  同样感知 v1 的恢复（只有 rootless **x86_64** 模拟器同时具备遗留系统
  调用和 link 拒绝 —— 物理设备是 aarch64，rooted 模拟器没有拒绝 —— 所以
  这个悬空一直隐藏着，直到该变体第一次运行，连带搞垮了下游 84 个
  `/etc/probe` 使用方，包括每一次 mt 线程掩码迭代）；以及 O_TMPFILE|O_EXCL
  magic-link 发布返回的是模拟的匿名源 EXDEV，而不是内核的 nlink-0
  ENOENT（SELinux 拒绝在内核的 nlink 检查之前触发）。

这两类差异都是 adb-shell 套件*运行位置*的产物，而不是生产的问题。
prod-env 层（见上文）在两个目标上以 `untrusted_app` 身份运行同样的行为
—— 从不是真正的 root，并且带有生产中的硬链接拒绝 —— 因此按放置规则，
与设备相关的断言属于那里；这些适配只为 adb-shell 运行中保留的 smoke
步骤而存在。
