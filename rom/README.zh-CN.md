# Wayondroid ROM 套件

[English](README.md) · 简体中文

ROM 在 Wayondroid 实例里运行所需的全部内容，和依赖它的引擎放在一起。

**状态：按引擎契约编写、在宿主上做过单元测试，从未在已启动的 Android ROM 里运行过。** 宿主测试中，引擎在真实内核上运行一个假的 init。这里的东西还没有启动过任何真实的 Android 系统镜像，所以首次启动是一个尚未达成的里程碑，而不是既成事实。

## 引擎提供给 guest 的环境

运行时以 PID 1 启动 ROM 的 `/init`（来自 `ramdisk.img`），此时以下内容已经就绪：

| 路径 | 说明 |
|---|---|
| `/system`、`/vendor` | ROM 的镜像，以只读方式 loop 挂载。 |
| `/data` | 首次启动时创建的稀疏 ext4 镜像。大小取实例的存储设置，只是实际磁盘占用的上限。 |
| `/cache` | 64 MB tmpfs。 |
| `/proc`、`/sys`、`/dev` | 全新挂载。`/proc/sys` 和 sysrq 只读，`/sys` 只读，`/dev` 是一个小 tmpfs，包含 null、zero、full、random、urandom、tty、fuse、pts、ptmx、console 和 binder 设备。没有 uevent 来源，所以 ueventd 无事可做。 |
| `/dev/binder`、`/dev/hwbinder`、`/dev/vndbinder` | 手机内核支持 binderfs 时为私有 binderfs 实例，否则使用已存在的 Anbox binder 设备。 |
| `/vine` | 实例的共享目录，见下文。 |
| 网络 | 独立的 network namespace，`eth0` 已配置地址（`10.77.<slot>.2/30`）和默认路由，由宿主做 NAT。 |

容器有独立的 PID、UTS、IPC、network 和 mount namespace 以及一个 cgroup。它**没有 user namespace**：guest 的 root 就是手机的 root。威胁模型见文档仓库。

## 共享目录 `/vine`

| 文件 | 方向 | 含义 |
|---|---|---|
| `boot.prop` | 宿主 → guest | `ro.boot.vine.*` 属性（instance、width、height、dpi、ram_mb、root、qemu32、net_ip、net_gw、net_dns）。引擎还会把它们合并进 ramdisk 的 `default.prop`，并以 `androidboot.vine.*` 写入 `/proc/cmdline`。 |
| `vinefb` | guest → 宿主 | 显示输出。4096 字节头部，之后是像素。用 `hwc/vinefb_writer.h` 写入。 |
| `agent.sock` | 宿主 → guest | guest agent 监听的 Unix socket。负责输入、App 列表、启动 App、图标、剪贴板。 |
| `boot_completed` | guest → 宿主 | Android 启动完成时由 guest 创建。宿主看到它出现后把实例标记为运行中。 |

## 套件内容

| 路径 | 用途 |
|---|---|
| `INIT_PATCH.md` | 原版 Android `init` 要在容器里运行需要做的修改，包括引擎要求的标记字符串。 |
| `init/init.vine.rc` | ramdisk 的 `/init.vine.rc`。启动 agent 和网络修复脚本，并在 `sys.boot_completed=1` 时写入 `boot_completed`。 |
| `system/etc/vine/net.sh` | 引擎的网络配置缺失时，根据 boot 属性修复 `eth0`。 |
| `agent/` | guest agent。`AgentProtocol.java` 是纯 Java，有单元测试；`VineAgent.java` 是 Android 侧（输入注入、PackageManager、剪贴板）。`build_agent.sh` 生成 `vine-agent.jar`。 |
| `hwc/vinefb_writer.h` | 显示契约的纯头文件写入端，已对照宿主读取端测试。 |
| `package_vrom.py` | 把 `ramdisk.img`、`system.img` 和可选的 `vendor.img` 打包成 `.vrom`。`--kind waydroid` 打包不带 ramdisk 的 system-as-root 镜像（Waydroid 的 system 和 vendor 镜像），App 用 rootless 引擎启动它（`docs/rootless.md`）。 |

## 组装 ROM

1. 按 `INIT_PATCH.md` 修改 `init`，重新构建 ramdisk。
2. 构建 agent：`ANDROID_HOME=$HOME/Android/Sdk sh agent/build_agent.sh`，然后把 `agent/build/vine-agent.jar` 复制到 system 镜像的 `/system/framework/`。
3. 把 `init/init.vine.rc` 放到 ramdisk 根目录作为 `/init.vine.rc`（引擎发现这个文件时会把 `ro.hardware` 设为 `vine`，`init` 因此会加载它），并把 `system/etc/vine/net.sh` 复制到 system 镜像的 `/system/etc/vine/`。
4. 让 ROM 的图形栈通过 `vinefb::Writer` 写出完成的帧（例如在一个小型 hwcomposer 或 gralloc shim 里，或在软件渲染器的 present 调用里）。
5. 打包：`python3 package_vrom.py images/ MyROM.vrom --name "My ROM" --android-version 13 --api-level 33`。
6. 在 App 中导入，创建实例并启动；启动失败时在实例页面打开 Show diagnostics 查看启动日志末尾。

## 已知未解决的问题

- **首次真实启动。** 这里没有任何东西能证明，在内核缺少原版 Android 10+ 用户态所期望的功能（binder、ashmem 或 memfd、正确的 cgroup）时，它能顺利走完 `init`。App 里的宿主探测会列出手机内核提供了什么。
- **SELinux。** 内核策略是手机的。guest `init` 若要加载自己的策略，除非带有标记，否则会被引擎拒绝；即使是打过补丁的 `init`，也必须跳过策略加载。
- **Android 自身的网络。** `eth0` 在内核层面可用，但只有 ROM 带有以太网支持时，Android 的 ConnectivityService 才会把它当作默认网络。`net.sh` 设置地址和 DNS 属性，但无法让框架接管这个接口。
- **显示。** 还没有 ROM 写入 `vinefb`。宿主读取端和 guest 写入端彼此匹配，目前只证明了这一点。
- **guest root 就是宿主 root。** 没有 user namespace，所以只运行你信任的 ROM。
