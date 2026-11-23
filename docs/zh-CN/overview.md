# Wayondroid 中文概览

Wayondroid（Waydroid on Android）在**未 root、SELinux enforcing** 的原厂 Android
手机上，用一个普通 App 跑起完整的第二个 Android。没有虚拟机（KVM/QEMU），也没有内核模块：
guest 的 `init`、`zygote`、`system_server`、SurfaceFlinger 和各个 App 都是宿主 App
自己的进程，跑在用户态容器运行时 **tawcroot** 和用户态 binder **binderd** 之上。

> 状态：alpha。只在 Galaxy S24（Exynos 2400，Android 17，arm64-only，SELinux enforcing）上测过。

## 为什么难

普通 App 只有一个 uid：不能建 namespace、不能 `mount`、没有 loop 设备，也拿不到给容器用的
`/dev/binder`。Wayondroid 的思路是把 guest 启动依赖的这些内核设施**在用户态模拟出来**；
只有模拟起来不合理的 Android 策略问题，才在 ROM 侧打补丁。

## 进程结构

```
Wayondroid App（单一 app uid，无 root）
├─ UI（Kotlin / Compose）、实例管理、画面显示（SurfaceView / SurfaceControl）
└─ libvine_helper.so --rootless
     ├─ binderd       用户态 binder / hwbinder / vndbinder
     ├─ vine_wl       Wayland 服务端，接 Waydroid 的 hwcomposer → 共享 vinefb
     ├─ vine_pulse    PulseAudio 服务端 → AAudio 播放
     ├─ vine_kumquat  gfxstream 宿主渲染器（开 GPU 加速时）
     └─ tawcroot --init -r <rootfs> -- /system/bin/init
          └─ guest：servicemanager、SurfaceFlinger、zygote64、system_server、App
```

## 核心组件

### tawcroot：seccomp + SIGSYS 容器运行时

源自 TAWC。和 `proot` 的区别在于不用 `ptrace`：用手写 cBPF 的 seccomp 过滤器，
只有需要模拟的系统调用返回 `SECCOMP_RET_TRAP`，在进程内由 SIGSYS handler 处理；
其余系统调用按调用号直接放行（5.11+ 内核的 seccomp action cache 会把它们标为常量 ALLOW，
完全不走过滤器）。因此没有 ptrace 那种每次 syscall 的跨进程往返。

模拟内容：root 身份与文件属主、mount 与 mount namespace、`/proc` 与 `/sys`、
设备节点、网络接口。细节见 [tawcroot/](tawcroot/)，过滤器结构见
[tawcroot/seccomp-filter.md](tawcroot/seccomp-filter.md)。

### binderd：用户态 binder 驱动

按 `drivers/android/binder.c` 的语义在用户态重新实现，同时提供 binder、hwbinder、
vndbinder 三个设备。guest 打开 binder 时拿到的 fd 就是一条连到 binderd 的
`SOCK_SEQPACKET` 连接（fd ≥ 1000，可 poll/epoll）；事务数据由 binderd 直接写进
与 guest 共享的只读 memfd 映射。详见 [rootless.md](rootless.md) 的 binder 一节。

### 图形

guest 用 Waydroid 自带的 `hwcomposer.waydroid`（Wayland 客户端）出图，交给 `vine_wl`。

- 开 GPU 加速：帧留在手机 GPU 上直接上屏（零拷贝，需 Android 10+）。
- 不开：guest 软件渲染。
- GPU 加速（实验性）：guest 的 Vulkan / GLES 经 gfxstream 驱动序列化，由 `vine_kumquat`
  在手机 GPU 上执行（gfxstream / rutabaga 来自 AOSP、crosvm）。

### guest 内的替身组件

需要 App 拿不到的内核特性的部分，在 guest 里换成小替身：

- `vine_lmkd`：超过实例 RAM 设置或手机内存紧张时，杀 guest 的缓存进程。
- vold shim：不用 FUSE 挂载共享存储 `/sdcard`。
- gralloc shim。

## ROM

直接用 Waydroid 官方 OTA 的 LineageOS 20（Android 13）`waydroid_arm64_only` 镜像
（VANILLA 或 GAPPS），用 `rom/package_vrom.py --kind waydroid` 打成 `.vrom` 导入即可，
不需要自己编 AOSP。步骤见 [README](../../README.zh-CN.md#获取-rom)。

选 Waydroid 镜像的原因：system-as-root、flattened APEX（无需 apexd 的 loop/dm-verity）、
输入不需要 uinput、`sys.use_memfd=true` 免 ashmem。见 [rootless.md](rootless.md)。

## 局限

- **只支持 64 位 guest App**：tawcroot 没有 AArch32 支持，rootless 引擎把
  `ro.zygote` 设为 `zygote64`。
- GPU 加速是实验性的，App 画面异常或崩溃时关掉。
- 偶发启动卡住，停止后重启实例可恢复。
- 所有 guest App 都是 Wayondroid 的进程，和手机共享内存。
- 只在一台机型上测过；其他厂商 ROM 在 seccomp、memfd、SELinux 策略上的差异可能导致问题。
- 目前需要自己用 Android SDK/NDK 构建（`./gradlew :app:assembleDebug`）并打包 ROM。

完整列表见 [README](../../README.zh-CN.md#已知局限)。

## 延伸阅读

- [rootless.md](rootless.md)：rootless 引擎设计
- [rootless-boot.md](rootless-boot.md)：启动需要什么、为什么
- [tawcroot/](tawcroot/)：运行时内部实现
