# tawcroot — 构建与安装器集成

## 构建集成

在 `scripts/build-proot.sh` 旁边增加一个 `tawcroot/build.sh` 脚本：

- 用 NDK 的 clang 为 `arm64-v8a` 和 `x86_64` 交叉编译。
- 生产源码为纯 C11 + 少量 `.S` 文件。没有 talloc，没有 autoconf，没有
  config.h。
- **静态链接、非 PIE、freestanding**
  （`-static -nostdlib -nostartfiles -no-pie -ffreestanding`）。
  re-exec 架构（在我们的 `_start` 之前没有 bionic 链接器）和 seccomp 过滤器
  基于 IP 的 stub 白名单（跨 re-exec 地址稳定 — 见 §"为什么用 non-PIE"）都要求如此。
  handler/运行时目标文件还使用 `-fno-stack-protector`，且不加 sanitizer/profiling
  插桩，这样 guest 的 glibc 与 tawcroot 的运行时就不会通过编译器插入的辅助路径
  发生冲突。没有 `dlopen`，没有 Android property/IPC 功能（我们不使用这些）。
  目前二进制约 30 KB；手动 ELF 加载器和完整 handler 落地后约 1–2 MB。
- 通过 `-Wl,--image-base=` 为每个架构设定**固定基址**：
  0x2000000000（aarch64），0x40000000（x86_64）。NDK lld 直接拒绝
  `-Ttext-segment`；使用 `--image-base`。x86_64 的基址受静态 bionic 的 PLT32
  weak-undef 重定位约束 — 完整原因见 §"为什么用 non-PIE"。aarch64 的基址沿用 proot 的
  `LOADER_ADDRESS` 约定。
- 输出 `build/tawcroot-<abi>/libtawcroot.so` 并 strip；testhost 变体位于
  `build/tawcroot-<abi>/libtawcroot-testhost.so` 或
  `build/tawcroot-host/tawcroot-testhost`。
- Gradle `packTawcroot` 任务把生产二进制复制到
  `app/src/main/jniLibs/<abi>/libtawcroot.so`。testhost 从不复制 — 它只被宿主上
  的 cleat 编排器使用，以及被 `tawcroot/test.sh --device` 通过 adb push 使用。
- 首次 `--abi=host` 运行时，以固定 commit 把 cleat 克隆到 `./deps/cleat/`
  （gitignore，与 `deps/libhybris/`、`deps/libxkbcommon/`、`deps/proot/` 模式
  相同）。固定的版本写在构建脚本中；升级它是一次有意的变更。cleat 自带 vendored
  STC — 我们*不*单独克隆 STC。**cleat 只构建进测试编排器**，不进入任何一个
  tawcroot 二进制。
- 用宿主工具链原生构建 cleat 编排器（`build/tawcroot-host/tests`，hosted glibc
  二进制）。传入 `--tests` 时也会针对 bionic 交叉构建 `aarch64`/`x86_64` 版本，
  输出到 `build/tawcroot-<abi>/tests`。Android 变体就是 `tawcroot/test.sh
  --device` 在设备上运行的那个 — 与宿主构建相同的源文件集、相同的过滤语法、相同
  的退出码语义（cleat 是纯 POSIX C + vendored STC；没有仅限 glibc 的依赖）。见
  §"测试策略"。
- **宿主构建是增量的**，通过 `tawcroot/Makefile`：
  `gcc -MMD -MP` 跟踪头文件依赖，`-j$(nproc)` 并行编译。热重建（无源码改动）约
  30 ms；修改一个生产源文件会重建并重新链接两个 tawcroot 二进制，约 300 ms；修改
  一个 cleat 头文件只重建包含它的测试文件。紧凑的内循环中可以直接调用
  `make -C tawcroot` — `tawcroot/build.sh --abi=host` 在此之上额外加了克隆 cleat
  的步骤。交叉 ABI 的 NDK 构建仍留在 bash 流程中（NDK 的配置是 bash 形态的，而且
  它们不在内循环中）。

这与现有 proot 构建保持一致（proot 以 `libproot.so` + `libproot-loader.so`
发布；tawcroot 生产版以 `libtawcroot.so` 发布，`tawcroot-testhost` 和 `tests`
作为仅限宿主的测试产物）。

### 源文件列表存在于两处

生产 `.c` 集合在 `tawcroot/build.sh`（`SRC_C_PROD`，用于 NDK 交叉构建和设备上
测试）和 `tawcroot/Makefile`（`PROD_C`，用于 `tawcroot/test.sh --host` 所用的
宿主构建）之间重复。**新增一个 `.c` 文件意味着两处都要改。**

拆分是有原因的 — 宿主 Makefile 用 gcc 做头文件依赖跟踪以实现快速增量构建；交叉
构建需要 NDK 风格的 bash，放进 Makefile 会让它变乱 — 但这是一个正确性陷阱。
chroot.c 的回归正是如此：它被加进了 `PROD_C` 但漏了 `SRC_C_PROD`，于是
`tawcroot/test.sh --host` 通过了（宿主构建是完整的），而设备上发布的二进制甚至
不包含 chroot handler。（如果交叉构建跑了，本会出现链接错误，但 Gradle 的
`buildTawcroot` 任务没有把源码目录列为输入，跳过了重建 — 于是修复前的陈旧二进制
一直留在 jniLibs 中。）

缓解措施：Gradle 的 `buildTawcroot$abi` 现在把 `tawcroot/src` 和
`tawcroot/include` 列为输入（这样只改源码也会使缓存失效），并且如果引用的符号
缺失，交叉构建*确实*会在链接时显式失败 — 只要它真正运行。如果两个列表再次不一致，
`tawcroot/test.sh --device` 是发现它的标准方式：运行设备上测试会强制为目标 ABI
交叉构建，任何链接错误都会立即暴露。

## 安装器集成

在 `ProotMethod.kt` 旁边新增一个 `TawcrootMethod.kt`。形态相同：

- 在 `startInside` 中构建 argv（bind 表 + chroot exec）。
- 使用与 `ProotMethod` 相同的 bootstrap 缓存 + tar 解包流程。
- 没有 `/dev/shm` 宿主 bind — SIGSYS handler 通过 `memfd_create` 模拟 POSIX shm
  （`src/shm.c`）。见 §"引导与入口"。
- 运行 `TawcInstaller.installInto`，把 glvnd vendor JSON 和其他共存文件复制进去。
  （libhybris 目录树本身是位于 `/usr/lib/hybris/` 的只读 bind —
  notes/installation.md "Copy vs bind"；写这段时它和其他东西一样是复制的。）

Kotlin 的 `InstallationMethod` 枚举已经有一个 `extra` 槽（`metadata.json`）；我们
新增 `TAWCROOT` 取值，并在有足够把握后，让 `InstallActivity` 中的单选项在无 root
设备上默认选中它。

`scripts/rootfs-run.sh` 读取 `metadata.json` 来决定使用 chroot/proot/tawcroot
（目前只有 chroot/proot）。在 `case method` 分支中再加一个 case 即可。

