# tawcroot — 基于 systrap 的快速 rootless chroot

随运行时一起从 TAWC（commit 1f6368a）导入。本目录之外指向
`notes/*.md` 的链接指的是 TAWC 的 notes
（https://github.com/yoro1836/tawc/tree/main/notes）。

`tawcroot/` 是一个从零开始用 C 实现的 proot 式伪 chroot，使用
**seccomp `RET_TRAP` + 进程内 `SIGSYS` 处理**（即 gVisor 在其 platform
层使用的 "systrap" 技术），而不是 `ptrace`。目标是成为 proot 在 TAWC
用途上的严格超集 —— 兼容范围相同，但系统调用开销显著更低、兼容性
hack 更少，代码结构按我们的需求设计，而不是继承自 proot 的 ptrace
传统。

**状态：默认且唯一官方支持的安装方式。**
Release 构建只包含 tawcroot；chroot/proot 仅供开发使用，只出现在
debug 构建中。构建期的开关控制见 `notes/installation.md` "Install methods"。

本文档是设计 + 实现计划。代码正在从零编写。当 proot 解决的某个问题
不显而易见时，参考 `deps/proot/`（我们当前 vendor 的 Termux fork）——
尤其是其 `src/tracee/seccomp.c` 中的 Android 特有问题以及 loader/exec
流程 —— 但**不要移植 proot 的架构**。proot 从其 ptrace 传统中继承了
tracer/tracee 设计，这与单进程 systrap 模型不契合，而且其代码质量
参差不齐，这些我们都不想继承。

## 一段话说明它是什么

一个小型 C 程序：给定 rootfs 路径和命令，它设置一个 seccomp-bpf
过滤器，拦截携带路径的系统调用以及少数其他系统调用；安装一个
`SIGSYS` handler，针对 rootfs 模拟这些系统调用；并通过重新 exec +
进程内 ELF loader 启动 guest，使 handler 保持安装状态。路径转换、
fake-root 的 uid/stat 行为以及 Android 特有的系统调用修正全部在进程内、
在发起该系统调用的同一线程中完成。没有 tracer 进程，没有 `ptrace`，
没有每次系统调用的上下文切换。


## 目录

本设计 + 实现说明分为以下文件：

- [overview.md](overview.md) — 语言选择、为何不用 proot、扩展目标，以及 tawcroot 明确不做什么。
- [architecture.md](architecture.md) — 架构概览与进程布局。
- [sigsys-handler.md](sigsys-handler.md) — `SIGSYS` handler：guest 内存访问、保存的寄存器、发起宿主系统调用、non-PIE、线程/`vfork`、信号/seccomp 控制、异步信号安全、编码约定。
- [path-translation.md](path-translation.md) — 转换规则、哪些系统调用需要拦截、`execve`、`/proc/self/exe`、`/proc/<pid>/fd/<n>` 与 `getcwd` 的反向转换、chroot 模拟。
- [link-emulation.md](link-emulation.md) — 完整的硬链接模拟：`distros/<id>/tawcroot/` 链接存储格式、加锁/意图记录的崩溃安全、O_TMPFILE 发布、可接受的偏差。
- [seccomp-filter.md](seccomp-filter.md) — seccomp 过滤器与非 root 的过滤器安装。
- [systemd.md](systemd.md) — `--init --cgroup`：无 namespace 下以 systemd 作为 pid 1、伪 cgroup v2、挂载/cap 模拟。
- [bootstrap-and-modules.md](bootstrap-and-modules.md) — 引导与入口、模块布局。
- [build-and-install.md](build-and-install.md) — 构建集成与安装器集成。
- [testing.md](testing.md) — 测试策略与分层。
- [exec-performance.md](exec-performance.md) — exec 开销归因、测量约束，以及被否决的实验。
- [execution-techniques.md](execution-techniques.md) — 其他拦截与启动技术，及其可行性限制。
- [phasing.md](phasing.md) — 分阶段实现计划与历史。
- [status.md](status.md) — MVP 前的已知缺口、可接受的系统调用保真度偏差、后续工作、已确认的环境、维护约定。

树中其他位置按章节名引用这些文件（例如 `docs/tawcroot/path-translation.md §"chroot 模拟"`）；该标题位于主题与之匹配的文件中。
