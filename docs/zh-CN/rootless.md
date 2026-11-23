# 免 root 引擎：在 tawcroot 上运行 Waydroid 镜像

Wayondroid 目前需要 root：`libvine_helper.so` 在 `su` 下运行，因为
`unshare`、`mount` 和 loop 设备需要 `CAP_SYS_ADMIN`。本文档描述免 root
路径（路线图阶段 4）的结构，它取代原计划的 `proot` 兜底方案。

思路：用 **tawcroot**（`tawcroot/`，从 TAWC 引入并在本项目中继续开发的
seccomp/SIGSYS 免 root 运行时）启动 Android 镜像，在用户态模拟 binder，并
通过 Wayondroid 应用内的一个小型 Wayland 合成器绘制 guest。guest ROM 是
**Waydroid** 的 LineageOS 镜像，它本来就是为容器构建的。

经验法则：内核接口在 tawcroot 中模拟（适用于任何容器镜像，包括 redroid）。
Android 策略层面的问题在我们自己的 ROM 构建中打补丁，且仅限于模拟会显得
荒谬的地方。

## 为什么用 Waydroid 镜像

基于 `ota.waydro.id` 上的 `lineage-20.0-20260927-VANILLA-waydroid_arm64-system.zip`
和 `lineage-20.0-20261022-MAINLINE-waydroid_arm64-vendor.zip` 核对（OTA JSON
为每个文件列出了 sha256）。

- Android 13，userdebug，安全补丁 2026-02，可再分发（LineageOS）。
  原始 ext4：system 1.9 GB，vendor 0.4 GB。
- System-as-root：没有 ramdisk，system 镜像的根就是 `/`。
- **扁平化 APEX**（`/system/apex/<name>/` 目录）：没有 apexd 的
  loop/dm-verity。
- `hwcomposer.waydroid` 是一个 Wayland 客户端，具有 `wl_shm`、dmabuf 和
  `android_wlegl` 三种缓冲区路径，以及可选的每任务一窗口模式。
- 输入不需要 uinput：HWC 把 Wayland 输入写入 `/dev/input/wl_*_events`
  供 InputFlinger 读取。
- 宿主配置只有一个文件 `/vendor/waydroid.prop`（`sys.use_memfd=true`
  免去了对 ashmem 的需求）。
- 注意：`ro.zygote=zygote64_32`。tawcroot 不支持 AArch32，因此在我们自己的
  纯 64 位构建出现之前，免 root 引擎设置 `ro.zygote=zygote64`（仅 64 位
  应用）。

## 进程布局（免 root）

```
Wayondroid app process (app uid)
├─ Kotlin UI, VineVMManager, HelperClient
├─ libvine_runtime.so: DisplayBridge draws <instance>/shared/vinefb
└─ libvine_helper.so --rootless                      (same line protocol, no su)
     ├─ per instance: rootfs preparation (a child, once per boot)
     ├─ binderd: binder / hwbinder / vndbinder
     ├─ vine_wl: Wayland server on run/wayland-0 ──► shared/vinefb,
     │    input lines on <instance>/rootless/input
     ├─ vine_pulse: PulseAudio server on run/pulse/native
     ├─ vine_kumquat (GPU setting on): gfxstream host renderer, see "GPU"
     └─ keeper (subreaper of every guest process)
          └─ tawcroot --init --all-caps --cgroup --owners -r <rootfs> … -- /system/bin/init second_stage
               └─ servicemanager, hwservicemanager, surfaceflinger,
                  composer HAL (hwcomposer.waydroid) ──► wayland-0
                  zygote64 → system_server → apps
```

免 root 引擎（`helper/rootless_server.cpp`、
`rootless/rootless_instance.cpp`）是 helper 协议的第二个实现。
`VineVMManager` 为每个引擎保留一个 `HelperClient`：ROM 中没有 `ramdisk.img`
的实例（system-as-root 的 Waydroid 镜像，`package_vrom.py --kind waydroid`）
交给 `libvine_helper.so --rootless`，以应用身份直接启动；其余的交给 `su`
下的 root helper。

协议的映射方式：

| 命令 | 免 root 引擎 |
|---|---|
| `start` | 在子进程中准备 rootfs（首次启动时解包，[rootless-boot.md](rootless-boot.md)），然后启动 binderd、vine_wl、vine_pulse 和 init；init 的 rc 写入 `/run/user/1000/boot_completed` 后即为 `running` |
| `stop`、`kill` | 向 keeper 发 SIGTERM，keeper 杀掉其整棵进程树（包括 zygote 和应用），然后是各服务端 |
| `pause`、`resume` | 向 keeper 之下的每个进程发 SIGSTOP/SIGCONT |
| `agent_send` | 该行发往 vine_wl 的输入 socket；没有 evdev 码的按键（APP_SWITCH）通过 guest 的 `input keyevent` 发送 |
| `agent ping` | 应答 ok（输入可用）；其他 agent 请求失败：没有 guest agent |
| `exec` | `tawcroot -r <rootfs> --owners <binds> -- /system/bin/sh -c 'exec "$@"'`，环境变量取自 `init.environ.rc` |
| `apps` | 用 `cmd package query-activities` 获取启动器 activity；标签为包名 |
| `launch` | `cmd package resolve-activity`，然后 `cmd activity start-activity` |
| `icon`、快照 | 尚不支持 |

每个实例在 `<instance>/rootless/` 下有：`rootfs/`（解包出的镜像，
`rootfs.stamp` 记录是哪些镜像）、`tawcroot/`（store 和属主表）、`data/`
（guest 的 `/data`）、`binderfs/`、`run/`（`/run/user/1000`）、`input`、
`logs/`（`prepare`、`init`、`binderd`、`vine_wl`、`vine_pulse`；`diag` 显示
它们的末尾部分、kmsg 和 logcat）。system 或 vendor 镜像变化后会重新解包，
`/data` 和属主表也随之重新开始。镜像保留在实例目录中、与 rootfs 相邻（磁盘
占用约为 ROM 大小的两倍）。

## 仓库布局

| 路径 | 内容 |
|---|---|
| `tawcroot/` | 运行时（MIT，从 TAWC 引入）。宿主测试：`tawcroot/test.sh --host`。设计文档：`docs/tawcroot/` |
| `tawcroot/src/binder.c`、`tawcroot/include/binder_wire.h`、`tawcroot/binderd/` | binder 模拟（见下文） |
| `app/src/main/cpp/rootless/` | 免 root 后端：实例准备、进程监管、`waydroid.prop` 和 `/proc/cmdline` 生成、只读 ext4 解包器（`.vrom` 镜像 → rootfs 目录） |
| `app/src/main/cpp/helper/` | `helper_main` 新增 `--rootless`；`HelperServer` 分发到 root 的 `Container` 或免 root 引擎 |
| `app/src/main/rust/vine_compositor/` | 最小化的 Smithay 合成器（xdg-shell、`wl_shm`、`wl_seat` 触摸/键盘，之后加 `android_wlegl`），派生自 TAWC 的 `compositor/`（MIT），去掉了其启动器、Xwayland、ando 和远程部分 |
| `app/src/main/java/.../helper/HelperLauncher.kt` | root 或免 root 方式派生 |
| `app/src/main/java/.../domain/` | 引擎选择：默认免 root，在可用且被要求时使用 root |
| `rom/waydroid/` | guest 覆盖层：prop 模板（`ro.zygote=zygote64`、`sys.use_memfd=true`、GPU prop），启动 vine agent 的 init rc |
| `rom/build/`（以后） | 基于 Waydroid 源码树的 `local_manifests` 补丁，Hetzner 构建脚本 |
| `rom/package_vrom.py --kind waydroid` | Waydroid 的 system.img + vendor.img（从 OTA zip 解压）→ `.vrom`，无 ramdisk |

tawcroot 还会提供 Android 设备和 `/proc`/`/sys` 模拟，以及供 zygote 使用的
嵌套 seccomp 空操作。属主存储已完成（`--owners`，见下文）。

Gradle 在 `preBuild` 中运行 `tawcroot/build.sh`（NDK），并把
`libtawcroot.so`（之后还有 `libbinderd.so`）作为 jniLibs 打包，与
`libvine_helper.so` 使用的解包技巧相同。tawcroot 只支持 `arm64-v8a` 和
`x86_64`，因此免 root 引擎在 32 位 ABI 上不可用。

## Binder 模拟

已实现（里程碑 1）：`tawcroot/binderd/binderd.c`（驱动）、
`tawcroot/src/binder.c`（handler 内的垫片），线协议位于
`tawcroot/include/binder_wire.h`。通过把 binderd 的目录 bind 到
`/dev/binderfs`（`-b <dir>:/dev/binderfs`）启用；该目录包含 `binder`、
`hwbinder` 和 `vndbinder` socket 以及 libbinder 读取的 `features/` 文件。

- `open("/dev/{binder,hwbinder,vndbinder}")`（或 `/dev/binderfs/<name>`）
  连接到 binderd。guest 的 binder fd **就是**那条 SOCK_SEQPACKET 进程连接，
  位于 fd ≥ 1000，因此对它做 poll/epoll 是可行的（进程有待处理工作时
  binderd 保持一个 `BW_POLL` 字节在队列中），并且对它的 `close` 会被拦截。
- 对 fd ≥ 1000 的 `mmap` 会被拦截（一个新的 seccomp 叶子；匿名映射和普通
  文件仍然内联放行）。对 binder fd，它把接收缓冲区 memfd 以只读、共享方式
  映射；binderd 把事务直接写入其中。
- 每个 guest 线程都有自己的连接，以及一个与 binderd 共享的暂存 memfd，用于
  承载请求（BC 字节，然后是每个事务的数据、偏移和 PTR 缓冲区）和响应（BR
  字节、fd 修正）。fd 通过 `SCM_RIGHTS` 传递，落地时带 CLOEXEC，与内核安装
  它们的方式一致。
- 阻塞的读等待 {线程 socket, signalfd(guest 未屏蔽的信号)}。有待处理信号时
  发送 `BW_CANCEL`；ioctl 返回 `-EINTR` 并设置 `write_consumed`，libbinder 的
  `talkWithDriver` 会重试。
- 发送方 pid/euid 来自 tawcroot 的身份模拟和 `--init` pid 模拟，带内传递。
- binderd 遵循 `drivers/android/binder.c`：带 BR_INCREFS/ACQUIRE/RELEASE/DECREFS
  的 node/ref 引用计数、同步调用的延迟 TRANSACTION_COMPLETE、通过事务栈的
  嵌套调用路由、每 node 的 oneway 队列、死亡通知、BINDER/HANDLE/FD/FDA/PTR
  对象、`BR_TRANSACTION_SEC_CTX`（`--secctx`，默认 `u:r:untrusted_app:s0`）、
  BR_SPAWN_LOOPER、`SET_CONTEXT_MGR(_EXT)`、`GET_NODE_INFO_FOR_REF`、
  `GET_NODE_DEBUG_INFO`、`GET_EXTENDED_ERROR`。
- 未实现：优先级继承、冻结（`BINDER_FREEZE` 返回 `-EINVAL`）、oneway 垃圾
  检测、异步空间记账。
- 测试：`tawcroot/test.sh --host 'prod_binder.*'` 让两个 guest 进程
  （`tests/integration/programs/static_binder_check.c`）对接一个
  ASan/UBSan 构建的 binderd：secctx、epoll、带 FDA 的 oneway scatter-gather、
  fd 传递、binder→handle 翻译、嵌套调用、死亡通知，以及 manager 在嵌套调用
  中途死亡。
- 待办：helper 的宿主 API（注册 `waydroid*` 服务、调用
  `waydroidplatform`），以及在设备上运行真实的 libbinder（servicemanager）。

## GPU（graphics=gfxstream）

「GPU 加速」设置让 guest 的 Vulkan 运行在手机 GPU 上。由
`scripts/gpu/build-gpu.sh` 构建（放入 jniLibs）。

- **Guest：** Mesa 的 gfxstream-vk（`vulkan.gfxstream.so`，补丁位于
  `deps/mesa-patches/mesa`）以 kumquat 协议与 `vine_kumquat` 通信；ANGLE 在
  其上运行 GLES。`gralloc.vine`（`rootless/gralloc_gpu.c`）通过
  `vine-gralloc` socket 分配宿主 ColorBuffer，并分发 goldfish
  `cb_handle_t`，其宿主 handle 即资源 id。
- **宿主：** `vine_kumquat`（`gpu/kumquat`）是基于 `libgfxstream_backend.so`
  的 rutabaga kumquat 服务端，外加 gralloc 服务（`deps/rutabaga-patches`）和
  一个 AHardwareBuffer socket。
- **帧，零拷贝：** 设置 `ro.hardware.gralloc=vine` 时，hwcomposer 通过
  `android_wlegl` 发送每个缓冲区的 handle；vine_wl 把其资源 id 写入 vinefb
  头部（`gpu_resource`）；DisplayBridge 向 vine_kumquat 请求该 ColorBuffer
  的 AHB 一次（抽象 socket `gpu_ahb_socket()`、`tawc_gfxstream_lookup_ahb`），
  并在视图窗口的一个子 SurfaceControl 上显示它
  （`display/gpu_presenter.cpp`，Android 10+）。vine_wl 在下一个缓冲区替换
  当前缓冲区时释放它；guest 的下一次渲染与宿主合成器之间没有 fence，因此
  可能出现撕裂。

## 属主存储

`tawcroot --owners`（`tawcroot/src/owners.c`，布局见
`tawcroot/include/owners_table.h`）。宿主应用 uid 拥有每个文件，因此没有它
的话，切换 uid 的 guest 完全看不到属主信息。

- `<store>/owners`（store 为 `<rootfs>/../tawcroot`）是一个 24 MiB 的稀疏
  开放寻址表，(st_dev, st_ino) → (uid, gid)，由每个 guest 进程共享映射。
  stat 读取它无需系统调用；写入方对其加 flock。
- 无条目即为 root:root。非 root guest 创建的文件（open O_CREAT、mkdir、
  mknod、symlink）属于其 fsuid/fsgid。chown 按内核规则对虚拟身份生效：euid 0
  时可做任何更改，否则只允许属主把属组改为其所属的组之一。
- 删除 inode 的最后一个名字（unlink、rmdir、rename 覆盖它）会丢弃其条目，
  因此被复用的 inode 号重新从 root 开始。
- 一个会话标志（`TAWC_NS_OWNERS`）：跨 fork 和 exec 继承，默认关闭（TAWC
  风格的发行版会话保持现有行为）。
- 未完成：setgid 目录的属组继承、权限检查（见风险）、表增长超过 786k 个
  活跃条目（之后写入以 ENOSPC 失败，文件读出为 root）。
- 测试：`tawcroot/test.sh --host 'prod_owners.*'`。

## ROM

一个 `.vrom` 同时服务两个引擎：root 引擎 loop 挂载镜像，免 root 引擎按 ROM
把它们解包一次到共享的只读 rootfs 目录，每个实例另有可写的 `data` 目录。

- `ROMImage` 新增 `kind`（`vine`、`waydroid`）。
- root 引擎需要支持 system-as-root（目前它要求
  `ramdisk.img`，`container/namespace_manager.cpp:593`）。
- 解包器是只读 ext4（extent、线性和 htree 目录、快/慢符号链接、
  mode/uid/gid）。非 root 属主直接写入属主表
  （`tawcroot/include/owners_table.h`）。

## ROM 构建

仅在需要 ROM 补丁时进行。Hetzner Cloud，按小时计费：创建一台 CPX51 级服务器
（16 vCPU、32 GB RAM、360 GB 磁盘；加 swap），用我们的 `local_manifests`
同步 Waydroid 的 LineageOS 源码树，构建，拉取镜像，删除服务器。删除才会停止
计费，所以由脚本来做。只在频繁迭代期间保留快照。

## 启动流程（免 root）

1. 准备：解包 rootfs，写入 `/vendor/waydroid.prop`，合成 `/proc/cmdline`、
   `<instance>/data`、`<instance>/run/`。
2. 应用在 `<instance>/run/wayland-0` 上启动合成器。
3. helper 启动 binderd，然后
   `tawcroot --init --all-caps --cgroup --owners -r <rootfs> -b <instance>/data:/data -b <instance>/run:/run/user/1000 -- /init`
   （rootfs 准备和 bind：[rootless-boot.md](rootless-boot.md)）。
4. vine agent 报告启动完成、应用、图标，与 root 路径相同。

## 里程碑

1. ~~binderd + tawcroot 垫片，宿主测试：事务/回复、引用计数、fd 传递、死亡
   通知、scatter-gather。~~ 已完成。
2. ~~设备上：在 tawcroot 下运行 servicemanager + hwservicemanager +
   `service list`。~~ 已完成。
3. ~~应用内合成器，surfaceflinger + composer HAL → 第一帧。~~ 已完成：
   `vine_wl` 为 Waydroid 的 hwcomposer 提供服务，guest 启动到锁屏
   （[rootless-boot.md](rootless-boot.md)）。
4. system_server、启动器、输入；冻结 ROM 补丁清单。基本到位：system_server、
   SystemUI 和启动器运行，注入的触摸和按键可用；binderd 稳定性仍待解决。
   目前没有 ROM 补丁，只有 rootfs 准备（[rootless-boot.md](rootless-boot.md)）。
5. 在 helper 协议后面接入免 root 引擎、`.vrom` 解包、UI 切换。进行中：引擎
   和应用的引擎选择已完成并经过宿主测试（`tests/native/run_rootless_tests.sh`）；
   图标、应用标签和快照尚未完成。
6. 如果里程碑 4 需要补丁，则自行构建纯 64 位 ROM。

## 风险

- **幽灵进程杀手**：Android 12+ 在全系统范围内杀掉超过 32 个的应用子进程。
  一个启动完成的 guest 运行 60+ 个。很可能需要用户将其关闭（12L+：
  `adb shell settings put global settings_enable_monitor_phantom_procs false`）。
  对实际使用是阻塞性问题。
- **文件属主**：stat/chown 已完成（`--owners`），但权限检查未模拟：内核在每个
  文件上看到的都是应用 uid，因此一个应用 uid 可以读取另一个应用的 0700 文件。
  Android 启动过程中不应有任何东西依赖于被拒绝；guest 内的应用隔离不是安全
  边界。
- Binder 开销：每次调用都是 SIGSYS + socket 往返 + 两次拷贝。回退方案：在我们
  的 ROM 中给 libbinder 打补丁，使其直接与 binderd 通信。
- 网络：免 root 下没有 netns，而 ConnectivityService 需要一个由 netd 管理的
  默认网络。可能需要 ROM 补丁。
- FUSE 存储：免 root 下无法挂载 `/dev/fuse`。可能需要 ROM 补丁。
- 16 KB 页内核：guest 中 4 KB 对齐的 ELF 无法加载。
- 内存：启动完成的 Android 13 大约需要 1.5–2 GB。
