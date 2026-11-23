tawcroot — 基于 systrap 的快速免 root chroot

[English](README.md) · 简体中文

guest 默认身份是虚拟 root。`--host-user` 改用启动进程的 UID 和 GID，用于桌面会话和
Unix socket 凭据认证。两种模式都不授予 Android 特权。ARM64 的 namespace 启动模型是一个
不完整的兼容层，不是内核隔离。应用额外安装的 seccomp 过滤器会被报告为不支持。Android 自身的
沙箱仍然有效；tawcroot 不能替代安全边界。

目录结构：
- `src/`、`include/`              — 生产 C 源码（不依赖 libc，freestanding）
- `tests/{unit,handler,integration}/` — 由 cleat 编排的测试套件
- `tests/testhost/`               — 驱动测试的孪生二进制的源码
- `Makefile`                      — 宿主增量构建
- `build.sh`、`build-fixtures.sh` — 跨 ABI 的 NDK 构建脚本（也负责 Android 打包）
- `test.sh`                       — 在宿主上运行 cleat 编排器，或交叉编译后把编排器 +
                                    tawcroot + testhost + fixtures 推到设备上运行（`--device`）
- `perf/`                         — 比较各后端的微基准测试工具
- `binderd/`                      — 供 Android guest 使用的用户态 binder 驱动
                                    （../docs/zh-CN/rootless.md「Binder 模拟」一节）

设计与计划：../docs/zh-CN/tawcroot/README.md

导入自 TAWC（https://github.com/yoro1836/tawc，commit 1f6368a），
MIT 许可，见 LICENSE.MIT。此后在本仓库继续开发。
