# Rootless 调试启动脚本

[English](README.md) · 简体中文

通过 adb 在设备上迭代调试 rootless 引擎用的脚本。

暂存目录为 `/data/local/tmp/vine-dev/`（shell 用户）：

- `bin/`：`tawcroot` 和 `binderd`（`tawcroot/build.sh --abi=aarch64`，取自
  `app/src/main/jniLibs/arm64-v8a/`）、`ext4extract`（用 NDK 静态编译
  `app/src/main/cpp/rootless/ext4_extract.c`，带 `-DVINE_EXT4_EXTRACT_MAIN`）、
  `faultcatch`（用 NDK 静态编译 `faultcatch.c`）、`vine_wl`、
  `vine_pulse`、`vine_lmkd` 和 `vine_hal_service`（App 的 CMake 目标
  `libvine_wl.so`、`libvine_pulse.so`、`libvine_lmkd.so`、
  `libvine_hal_service.so`；最后一个会 dlopen() 镜像里的 libhidlbase）。
- `w/rootfs`：用 `bin/ext4extract system.img w/rootfs --owners w/tawcroot/owners`
  解出的 Waydroid system 镜像，vendor 镜像以同样方式解到 `w/rootfs/vendor`。

shell 用户不能在 `/data/local/tmp` 下创建 socket 文件，所以在那里 binderd 以
`--abstract` 运行（`BINDERD_ARGS="--abstract"`）。

`run-sm.sh`：启动 binderd + servicemanager，然后执行 `service list`。

## App 上下文

完整启动和真实引擎一样以 app uid 运行：用 `run-as` 进入一个没有代码的可调试包
（`me.vine.dev`），在它的数据目录下放同样的目录树（`bin/`、`w/`）。从暂存目录把文件复制进去
（`run-as me.vine.dev cp /data/local/tmp/vine-dev/bin/X bin/`）。

`boot-init.sh`：准备 rootfs（见 docs/zh-CN/rootless-boot.md），启动 binderd、在 Waydroid 的
init（第二阶段）上运行 `tawcroot --init`，并启动 guest 的 `logcat`。输出：`w/init.log`、
`w/logcat.txt`、`w/rootfs/dev/kmsg`（以 NUL 分隔）、`w/binderd.log`。

它还会启动 `vine_wl`（帧写入 `w/vinefb`：4096 字节头部，之后是 RGBA 行），并在结束时杀掉所有
guest 进程，包括被重新挂到别的父进程下的 zygote。

可调参数：`T`（运行秒数，默认 20）、`WRAP=bin/faultcatch`（让 init 和 binderd 在崩溃转储器下
运行；`BINDERD_WRAP` 只作用于 binderd）、`BINDERD`（换一个 binderd 构建）、
`BINDERD_FLAGS`（`-v`）、`CGROUP_FLAG=`（去掉 `--cgroup`）、`LOGCAT_DELAY`、
`KMSG_LINES`、`SIZE` 和 `DENSITY`（显示）、`DIAG`（拆除前在 guest 里执行的 shell 命令，输出写入
`w/diag.txt`）。

对 binderd 执行 `kill -USR1` 会把每个进程、其线程及它们的事务栈记录到 `w/binderd.log`。
