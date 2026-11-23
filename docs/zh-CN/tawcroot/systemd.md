# systemd 作为 pid 1（`--init --cgroup`）

目标：在没有 root、user namespace 或 `unshare` 的情况下，把 guest 的
systemd 作为 pid 1 启动。下文所述的一切都是 tawcroot 内部的模拟；没有
任何内核隔离。

```
tawcroot -r <rootfs> -b /proc:/proc -b /sys:/sys -b /dev:/dev \
    -b <dir>:/run -b <pty>:/dev/console --init --cgroup -- /lib/systemd/systemd
```

在环境中设置 `container=<name>`，让 systemd 走容器路径。`/dev/console`
必须是 tty，否则 systemd 不输出任何日志。

## Pid 1（`--init`）
PID 过滤器和 subreaper 见 [seccomp-filter.md](seccomp-filter.md)。在此
之上，init 会话中：
- `/proc/1/...` 解析为 init 真实的 `/proc/<pid>`，并且
  `/proc/<pid>/stat|status`（以及 `self`、`task/<tid>`）被重写，使 init 的
  pid/ppid/pgrp/session/`NSpid` 显示为 1，其 ppid 显示为 0。systemd 用这种
  方式检查子进程的 ppid。
- 伪造的 capability（`capget`/`capset`）从 systemd-nspawn 的默认集合开始
  （`0xfdecafff`，bounding set 相应收缩），`PR_CAPBSET_READ/DROP`、
  `PR_CAP_AMBIENT`、`PR_SET_SECUREBITS` 针对影子状态成功；
  `/proc/<pid>/status` 的 `Cap*` 行显示该状态。`--all-caps` 则从完整集合
  开始（Android 的 init 会中止 capset 被拒绝的服务，例如 logd 的
  CAP_SYSLOG）。Securebits 也被影子化（`PR_{GET,SET}_SECUREBITS`、
  `PR_{GET,SET}_KEEPCAPS`，遵循内核的锁定规则；exec 清除
  `SECBIT_KEEP_CAPS`）：宿主应用的 securebits 是锁定的。
- 宿主因提升硬限制而拒绝的 `setrlimit`/`prlimit64`，对持有伪造
  CAP_SYS_RESOURCE 的进程会被钳制到宿主的硬限制（bpfloader 的 1 GiB
  `RLIMIT_MEMLOCK`）。
- 看起来像一个 user-namespaced 容器：`uid_map`/`gid_map` 读出
  `0 <uid> 1`，`setgroups` 读出 `deny`，`/proc/*/ns/user` 的 stat 结果带有
  非 init 的 inode。于是 systemd 跳过它在那里无法运行的 unit（debugfs、
  tracing、fusectl、hugepages、binfmt_misc）。`/proc/sys` 的 `statfs`
  报告 `ST_RDONLY`。
- init 的直接子进程获得 `PR_SET_PDEATHSIG(SIGKILL)`（在 fork 时和 exec 时，
  因为 systemd 通过 `posix_spawn` 派生），因此服务随 init 一起退出，而不是
  比它活得更久。
- Pid 隔离：信号和 `/proc` 列表只到达 init 及其后代
  （[seccomp-filter.md](seccomp-filter.md)）。guest 的 `kill(-1)` 不再波及
  应用进程。
- `unshare` 返回 `EPERM`，systemd 将其理解为「已容器化」并跳过服务沙箱。

## 挂载
代码：`src/mounts.c`。会话有一张挂载表，是位于 `<store>/mounts` 的一块
flock 保护的共享 mmap（启动时清空，与 `<store>/mnt/` 相同）。在挂载表的
generation 变化后，每个进程在下一次路径翻译时把它镜像到自己的 bind 表中
（带 `mount_id` 的 bind）。
`unshare(CLONE_NEWNS)`（隐含 `CLONE_FS`）把当前表复制到
`<store>/mntns/<pid>.<seq>` 并把进程移过去；fork 出的进程共享它，exec 通过
`tawc_namespace.mnt_ns` 重新关联。继承来的条目被标记为共享，因此在一个
namespace 中 umount 时，会保留另一个 namespace 仍在使用的 tmpfs 后备目录；
namespace 新建的后备目录命名为 `mnt/<pid>.<seq>-<id>`。一切都是 private
的：namespace 之间没有传播（zygote 的 `MS_SLAVE` 会被接受，但没有任何
含义）。
`CLONE_VM` 子进程（vfork、`posix_spawn`）共享父进程的 bind 表但不共享其 fd，
因此它从不做镜像；在它的 exec 构建自己的视图之前，它经由父进程的视图路由。
- `tmpfs`/`ramfs`/`mqueue` 把一个新的后备目录 `<store>/mnt/<id>`（`mode=`
  生效）挂载到目标上；`statfs` 报告对应的 fs magic。父目录存在而目标不存在
  时也能工作（宿主 bind 的 `/dev` 中的 `/dev/mqueue`）。
- `MS_BIND`（文件也可）、`MS_MOVE` 和 `MS_REMOUNT`（`ro` 切换）作用于挂载表；
  `umount2` 删除一个条目（tmpfs 的后备目录被分离到 `mnt/.dead-<id>`），对
  非挂载点返回 `EINVAL`，以便 umount 循环能结束。仅涉及传播的调用以及在
  API 挂载点（`/proc`、`/sys`、`/dev`、`/dev/shm`、`/dev/pts`、`/run`、
  `/sys/fs/cgroup`）上的挂载返回 0；其他文件系统类型返回 `EPERM`。
- 以 `/proc/self/fd/N` 形式给出的目标（systemd 的风格）通过该 fd 解析。
- `/proc/mounts` 和 `/proc/<pid>/mountinfo|mounts` 由命令行 bind 加上挂载表
  合成。libmount 的监视器对 mountinfo 做 epoll；这些 fd 位于保留 fd 下限
  之上，在那里一个叠加的过滤器拦截 `epoll_ctl`，改为注册一个监视
  `<store>/mounts.event` 的 inotify fd，于是 systemd 能看到挂载变化。
- 新挂载 API（`fsopen`、`open_tree`、`move_mount`、……、`mount_setattr`）以及
  `statmount`/`listmount` 返回 `ENOSYS`；systemd 和 util-linux 回退到
  `mount(2)`。

缺口：没有挂载传播，没有 `setns`/`/proc/<pid>/ns/mnt`，没有
`clone(CLONE_NEWNS)`；namespace 表在会话期间从不回收；模拟的挂载对会话外
的进程不可见。

## 伪造的 cgroup v2（`--cgroup`）
代码：`src/cgroup.c`。需要 `--init` 风格的启动；在 guest 启动前于 main 中
建立。
- 一个真实目录 `<store>/cgroup`（store = `<rootfs parent>/tawcroot`）在启动时
  清空并 bind 到 `/sys/fs/cgroup`。`mkdir` 创建接口文件（`cgroup.procs`、
  `cgroup.threads`、`cgroup.controllers`（空）、`cgroup.subtree_control`、
  `cgroup.events`、`cgroup.type`、`cgroup.freeze`、`cgroup.kill`、……）。
  `rmdir` 删除它们，在仍有进程或有子 cgroup 时以 `EBUSY` 失败。
- 成员关系保存在 `<store>/cgroup.registry` 中：一块 flock 保护的 pid →
  cgroup 路径共享 mmap，每个条目都与该 pid 的 starttime 校验，因此被复用的
  pid 会被剔除。被拦截的 fork/clone 把子进程放入父进程的 cgroup；跳过了被
  拦截 fork 的 exec（vfork）会自行注册。
- 读取 `cgroup.procs|threads|events` 返回一个由 registry 构建的 memfd。
  `cgroup.procs|threads|kill` 的写方式 open 被移到一个保留 fd（≥1000），
  一个叠加的 fd 下限过滤器只对这些 fd 拦截 `write`，因此普通写入不被拦截。
  写入一个 pid 即移动它；向 `cgroup.kill` 写 `1` 会 SIGKILL 整个子树。
- `/proc/<pid>/cgroup` 合成为 `0::/<path>`；树下的 `statfs` 报告
  `CGROUP2_SUPER_MAGIC`。

缺口：没有 controller 或限制；`cgroup.events` 只在移动时刷新，进程退出时
不刷新；`name_to_handle_at` 的 cgroup ID 未模拟（systemd 忽略该错误）。

## Loader 与 MDWE
带 `MemoryDenyWriteExecute=` 的 unit 会获得 `PR_SET_MDWE`，它跨 exec 继承。
因此 loader 从不把代码段映射为可写：与 binfmt_elf 一样，它只在段有 BSS 时
才把段的文件尾部清零。

## 主机名、端口、对端
- 主机名（`src/uts.c`）：`sethostname`/`setdomainname` 写入
  `<store>/uts`，由会话共享；`uname` 和
  `/proc/sys/kernel/{hostname,domainname}` 读取它。
- 特权端口（`src/ports.c`）：guest 端口 p < 1024 对应宿主端口 p + 20000。
  bind 对任意地址都做偏移；connect/sendto/sendmsg 对回环目的地址做偏移；
  getsockname/getpeername/accept 以及 recvfrom/recvmsg/recvmmsg 的源地址
  偏移回来。inet 数据报 socket 被移到保留 fd 下限之上，在那里一个叠加的
  过滤器拦截它们的接收。未覆盖：`sendmmsg` 的目的地址，以及宿主上位于
  20000–21023 的真实服务会显示为未偏移的端口。
- 对端身份：guest 对端的 SO_PEERCRED 报告其虚拟 uid/gid。init 会话中的
  进程在 id 变化时把它们记录到 `<store>/ids/<pid>`（与 starttime 校验）；
  没有记录的对端使用最近的有记录祖先的 id。dbus-daemon 需要这一点才能让
  服务拥有它们的总线名。
- `capget` 像内核一样应答 libcap-ng 的版本探测（写入 v3 版本，返回
  `EINVAL`）。

## 已知的启动失败

Debian sid 的 systemd 257 能启动到 `graphical.target` 且没有失败的 unit：
journald、udevd、networkd、resolved、logind、dbus、hostnamed 和
console-getty 运行，`busctl`/`hostnamectl`/`resolvectl` 可用，`tmp.mount`、
`run-lock.mount`、`dev-mqueue.mount` 能挂载。
- 当 `/etc/machine-id` 未设置时，`systemd-firstboot` 会在控制台上等待；
  rootfs 需要完成首次启动设置，或者屏蔽该 unit。
- 指向宿主 bind 的 `/dev` 中的 `/run/initctl` 和
  `/run/systemd/journal/dev-log` 符号链接以 `EACCES` 失败（无害）；
  `getty@tty1` 无法打开宿主的 `/dev/tty1`。
- 把 `RLIMIT_NOFILE` 提升到应用硬限制之上会以 `EPERM` 失败（服务记录日志
  后继续）。
