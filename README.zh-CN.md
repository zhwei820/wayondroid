<div align="center">

# 🌿 Wayondroid

**Waydroid on Android：在一个普通 App 里运行第二个 Android，无需 root。**

[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)

[English](README.md) · 简体中文

</div>

Wayondroid 在未 root 的原厂 Android 手机上，于一个普通 App 内启动 Waydroid 的
LineageOS 20（Android 13）镜像。没有虚拟机，也没有内核模块：guest 自己的 `init`、
`zygote`、`system_server`、SurfaceFlinger 和各个 App 都作为本 App 的进程运行，
底层是用户态容器运行时（**tawcroot**）和用户态 binder（**binderd**）。

> **状态：alpha。** 在 Galaxy S24（Exynos 2400，Android 17，arm64-only，
> SELinux enforcing）上开发和测试，其他手机未测试。
> 会有不少粗糙之处，见[已知局限](#已知局限)。

## 已可用的功能

| 方面 | 状态 |
|---|---|
| 启动 | Waydroid LineageOS 20 `waydroid_arm64_only` 镜像（VANILLA 和 GAPPS）无需 root 即可启动到可用的桌面 |
| 显示 | guest 画面直接上屏。开启 GPU 加速时，帧始终留在手机 GPU 上（零拷贝，Android 10+）；否则 guest 使用软件渲染 |
| GPU | 实验性：guest 的 Vulkan/GLES 经 gfxstream 在手机 GPU 上运行 |
| 输入 | 多点触控、按键、文本输入。guest 画面打开时，手机音量键控制 guest |
| 音频 | guest 声音从手机扬声器播放（为 Waydroid 音频 HAL 提供的 PulseAudio 服务端，用 AAudio 播放） |
| 网络 | guest 通过手机的网络连接上网 |
| 存储 | 共享存储（`/sdcard`）已挂载：文件 App、文件和照片选择器可用 |
| Google 应用 | GAPPS 镜像可运行 Google Play 服务 |
| 内存 | 超出实例 RAM 设置或手机内存不足时，guest 会杀掉自己的缓存 App |
| 每实例设置 | RAM、存储大小、帧率（最高到手机刷新率）和分辨率 |
| App | Material You 界面、多实例、指向 guest App 的桌面快捷方式 |

## 要求

- 64 位手机（`arm64-v8a`）。tawcroot 也能构建 `x86_64`；32 位 ABI 只有 root 引擎。
- App 可安装在 Android 8.0 及以上，但引擎只在 Android 17 上测试过。零拷贝显示需要
  Android 10，让手机刷新率匹配 guest 需要 Android 11。
- 充足的空闲内存：guest 是一整个 Android，GAPPS 更甚。
- 存储：每个实例约为 ROM 大小的两倍（镜像加解压后的 rootfs），再加 guest 写入的数据。

## 获取 ROM

Wayondroid 启动 `.vrom` 文件：一个包含 `manifest.json`、`system.img` 和 `vendor.img`
的 zip。用 Waydroid 官方镜像制作：

1. 从 Waydroid 的 OTA 服务器下载一个 system 镜像和 vendor 镜像。
   使用 **`waydroid_arm64_only`** 构建（guest 只运行 64 位）：
   - system：`https://ota.waydro.id/system/lineage/waydroid_arm64_only/VANILLA.json`，
     需要 Google 应用则用 `.../GAPPS.json`
   - vendor：`https://ota.waydro.id/vendor/waydroid_arm64_only/MAINLINE.json`
     （同一个 vendor 镜像两者通用）

   每个 JSON 都列出了文件的 URL 和 sha256。
2. 把两者解压到同一个目录，使其包含 `system.img` 和 `vendor.img`。
3. 打包：

   ```sh
   python3 rom/package_vrom.py --kind waydroid IMAGES_DIR waydroid-lineage20.vrom \
       --name "Waydroid LineageOS 20" --android-version 13 --api-level 33
   ```
4. 在 App 的 **ROMs** 标签页导入 `.vrom`，然后用它创建实例。

首次启动要解压镜像并编译所有 App，需要几分钟；之后的启动会快很多。

## 构建

完整步骤（构建主机要求、依赖、GPU 组件、安装与使用）见 [编译与使用](docs/zh-CN/build-and-usage.md)。

```sh
./gradlew :app:assembleDebug
```

需要 Android SDK 和 NDK。Gradle 会构建 tawcroot 和 binderd（`tawcroot/build.sh`）
以及原生代码。GPU 相关组件（gfxstream 宿主渲染器、guest 的 gfxstream Vulkan 驱动、
`vine_kumquat`）由 `scripts/gpu/build-gpu.sh` 预编译到 `app/src/main/jniLibs/`。guest 的传感器
HAL 和 RIL 取自 Android 13 模拟器镜像，由 `scripts/sensors/fetch-hal.py`（需要 `debugfs`）放到同一目录；
不跑则 guest 没有传感器，也没有 SIM 卡。RIL 还要 `scripts/radio/build-overlay.sh`（需要 SDK
build-tools）生成的 overlay，guest 的 framework 才会用它。

宿主测试：运行时用 `tawcroot/test.sh --host`，引擎用 `app/src/main/cpp/tests/native/`
下的脚本（`run_rootless_tests.sh`、`run_wayland_tests.sh`、`run_display_tests.sh`、
`run_pulse_tests.sh`）。App 单元测试：`./gradlew :app:testDebugUnitTest`。

## 工作原理

```
Wayondroid app (one app uid, no root)
├─ UI, instance manager, display (SurfaceView / SurfaceControl)
└─ libvine_helper.so --rootless
     ├─ binderd       binder, hwbinder and vndbinder in userspace
     ├─ vine_wl       Wayland server for Waydroid's hwcomposer → shared vinefb
     ├─ vine_pulse    PulseAudio server → AAudio
     ├─ vine_kumquat  gfxstream host renderer (GPU acceleration on)
     └─ tawcroot --init -r <rootfs> -- /system/bin/init
          └─ the guest: servicemanager, SurfaceFlinger, zygote64,
             system_server, apps
```

- **tawcroot** 用 seccomp 拦截 guest 的系统调用，模拟 App 无权做的事：root 身份与文件属主、
  挂载与 mount namespace、`/proc` 和 `/sys`、设备、网络接口。
- **binderd** 基于 Unix socket 实现 binder 驱动。
- 在 guest 内，用小型替身组件取代需要 App 拿不到的内核特性的部分：`vine_lmkd`（内存回收）、
  vold shim（不用 FUSE 的共享存储）和 gralloc shim。

详见：[docs/zh-CN/overview.md](docs/zh-CN/overview.md)（中文概览）、
[docs/zh-CN/build-and-usage.md](docs/zh-CN/build-and-usage.md)（编译与使用）、
[docs/zh-CN/rootless.md](docs/zh-CN/rootless.md)（设计）、
[docs/zh-CN/rootless-boot.md](docs/zh-CN/rootless-boot.md)（启动需要什么，以及为什么）、
[docs/zh-CN/tawcroot/](docs/zh-CN/tawcroot/)（运行时）。

## 已知局限

- 32 位 guest App 无法运行（guest 只有 64 位）。
- GPU 加速是实验性的；App 画面异常或崩溃时请关闭。
- App 直接写入共享存储的文件，要等下一次媒体扫描后才会出现在图库和媒体列表中
  （文件 App 会立即看到）。
- 这类实例暂不支持快照和 App 图标。
- 偶尔启动会卡住；停止后重新启动实例即可。
- 每个 guest App 都是 Wayondroid 的进程，因此与手机共享内存：关掉其他重量级 App 可让 guest 更流畅。

旧的 root 引擎（真实的 namespace 和 loop 挂载，用于已 root 的手机）及其位于
[`rom/`](rom/README.zh-CN.md) 的 guest 套件仍在代码树中，但尚未成功启动过 ROM。

## 许可证

Wayondroid 采用 GNU General Public License v3.0 许可，见 [LICENSE](LICENSE)。
tawcroot 源自 TAWC（MIT）。

| 组件 | 许可证 | 用途 |
|---|---|---|
| [Waydroid](https://github.com/waydroid/waydroid) / [LineageOS](https://lineageos.org/) 镜像 | Apache-2.0 等 | Guest ROM |
| [gfxstream](https://android.googlesource.com/platform/hardware/google/gfxstream/)、[Mesa](https://www.mesa3d.org/) | Apache-2.0、MIT | GPU 加速 |
| [QEMU](https://www.qemu.org/) | GPL-2.0-or-later | 32 位模拟（root 引擎） |
| [Jetpack Compose](https://developer.android.com/jetpack/compose)、[Hilt](https://dagger.dev/hilt/)、[Room](https://developer.android.com/training/data-storage/room) | Apache-2.0 | App |

## 致谢

- **[Waydroid](https://github.com/waydroid/waydroid)**：本项目基于其可容器化的 Android 镜像和 hwcomposer
- **TAWC**（Tess's Android Wayland Compositor）：tawcroot 的起源
- **[Anbox](https://github.com/anbox/anbox)** 和
  **[redroid](https://github.com/remote-android/redroid-doc)**：更早的 Android 容器化工作
- **gfxstream / rutabaga**（AOSP、crosvm）：GPU 技术栈
