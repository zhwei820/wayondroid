# 硬链接模拟（link store）

Android `untrusted_app` SELinux 对 `app_data_file` 拒绝 `link(2)` — `file` 和
`lnk_file` 两个类都拒绝（已在 enforcing 模拟器、Android 16 / kernel 6.6 上验证）。
tawcroot 完整模拟硬链接：共享 inode、实时的 `st_nlink`、任意顺序 unlink、普通
文件的 NOFOLLOW/readlink 语义。本文记录已发布的设计、格式和行为（提炼自已完成
— 且之后已删除 — 的 plans/tawcroot-full-link-emulation.md）。
代码：`src/linkstore.c`、`include/linkstore.h`，handler 集成在
`src/syscalls_fs.c`（linkat/unlinkat/renameat2/stats/openat/readlinkat/
symlinkat/utimensat/fchownat）和 `src/syscalls_fd.c`（getdents64 d_type 改写）。

## 磁盘格式（store 版本 1）

```
distros/<id>/
  rootfs/           guest tree (mount root) — guest files only
  tawcroot/         tawcroot-owned emulation state ("the store")
    version         store format version, ASCII integer
    lock            fcntl-record-lock file (created once, never deleted)
    intent          single-slot crash journal (+ intent.new rename twin)
    link/<token>    link objects (hardlink cluster data; may be symlinks
                    — hardlink-of-a-symlink stores the symlink itself)
    link/<token>.cnt  refcount sidecars (fixed-width "NNNNNNNNNN\n",
                    one pwrite, the only count store)
    work/<token>.<role>  transient staged/parked entries; roles
                    .add/.del/.src/.dst per mutation type
    tmp/<ino>       linkable O_TMPFILE files awaiting publish
```

硬链接簇的每个 guest 名字都是一个符号链接，其目标是不透明字面量
`tawcroot:link:<token>`。token = 对象 inode 的十进制（宿主层复制导致 inode 重新
编号后发生冲突时加 `-<k>` 后缀）。解析器检测到该前缀后，以 store 的 `link/` fd
为基重新定位；`symlinkat` 拒绝以 `tawcroot:` 开头的 guest 目标（防伪造 — 伪造的
token 符号链接会成为幽灵引用者）。store **只**在顶层 prod 入口被发现
（`<rootfs>/../tawcroot`），并经由 exec_state（v5 `store_host_off`）传给
`--exec-child`，因为 guest chroot 会改变 rootfs 视图，但从不改变 store。

**备份/导出不变式：`distros/<id>/` 是单位。** 发行版导出/导入
（notes/installation.md "Export / import"）携带 `rootfs/` 和这个 store；它丢弃
`lock`、`intent.new`、`tmp/*` 和 `work/*`（被搁置的泄漏：计数只会偏高，不会偏低），
在一次恢复 spawn 之后若有待处理的 `intent`（宿主真实路径）则拒绝导出，并在导入时
拒绝 `intent`/`work/*` 条目。导入后 token 不再等于对象 inode；NEW 的 `-<k>` 冲突
后缀处理了这一点（`linkstore_new_token_collision_after_copy`）。单独的 `rootfs/`
不再是自包含的 — 硬链接的数据在 `tawcroot/link/` 中。安装管理本来就把发行版目录
当作单位；任何新功能（导出、迁移、宿主侧复制）也必须如此。
实例目录内的 bind 别名可以使用同一个 store，包括私有 home 目录。当 Android 拒绝
真实硬链接时，外部/共享的 bind 源返回 `EXDEV`：把实例自有的 token 放在那里会在
其他实例或宿主使用方那里留下悬空文件。经由 `/proc/self/fd/N/...` 的目录遍历使用
同一个 guest 解析器；裸 fd 链接保留内核语义。因此 Steam 的运行时准备可以共享文件
内容，而不必每次启动都复制。store 的发现使用规范的 rootfs 目录，而不是 rootfs
别名的写法。可执行文件名和 `$ORIGIN` 保留面向 guest 的硬链接名，而不暴露后端对象
的路径。

版本门槛：`version` 在配置时读取，并在每次获取锁时重新读取（guest 会比会话活得
更久）。比支持的版本更新 → DEGRADED：token *检测*保持开启，所有 store *变更*都
拒绝（link 回退为原始 EPERM），零损坏。来自 store 之前的模拟方案的 v1 兜底符号链接
（guest 绝对路径目标）不匹配 token 形式，继续表现为普通符号链接；不做迁移。

## 并发 + 崩溃安全（概要）

- 每次结构性变更都包在一个整文件 `fcntl(F_SETLKW)` 锁（SIGKILL 时由内核释放；
  fork/exec 会朝安全方向逃逸它）+ 一个进程内 futex 互斥锁之内。读者无锁。
- 单调不变式：存储的计数 ≥ 存活的引用者 — 在名字出现前递增，在名字消失后递减。
  崩溃窗口只会泄漏（多计），从不丢数据。
- 单槽 intent 日志（宿主真实路径），通过 rename 写入；恢复只依据 store 内部状态
  （`work/`、`link/`、intent），在触碰任何 guest 条目前验证它是匹配的 token 符号
  链接，从不根据记录的路径重建名字，并且是幂等的（恢复可以被杀掉后重跑）。在每次
  获取锁时运行，外加一个 O(1) 的会话启动检查。CLOBBER（rename 到一个模拟名字上）
  有意不使用 intent；sidecar 写入是单次定宽 pwrite，因此无锁读者和 SIGKILL 不会
  把它撕裂。
- sidecar 缺失/损坏：报告 `st_nlink` 2，从不删除对象（降级，数据安全）。
- kill 矩阵覆盖：`tests/hosted/test_linkstore_kill.c` 在每个变更的每第 k 个系统
  调用之前杀掉它，重跑恢复（恢复本身也在每个 j 处被杀），并穿插无锁的 guest
  rename。

## O_TMPFILE

- `O_TMPFILE|O_EXCL`（"永不 link"）：纯透传 — 允许创建匿名文件，fstat nlink 精确
  为 0，发布尝试在内核中失败（ENOENT），与真实情况一致。在 SELinux 直接拒绝 link
  的场景（未 root 的设备 shell）下，拒绝先于内核的 nlink-0 检查生效，模拟介入，
  无名源得到的是匿名源 EXDEV — 可接受的偏差。
- 不带 O_EXCL 的 `O_TMPFILE`：文件创建在 `tmp/<ino>`（先用字母前缀的唯一名创建，
  再 rename 为 inode 十进制），并返回其 fd。发布用的 linkat — 无论是
  `linkat(fd, "", dst, AT_EMPTY_PATH)` 还是
  `linkat(AT_FDCWD, "/proc/self/fd/N", dst, AT_SYMLINK_FOLLOW)` — 通过宿主路径
  检测到位于 tmp 的源，变为一次原子的 `renameat2(NOREPLACE)`（白得 linkat 的
  EEXIST；仍打开的 fd 继续引用新位置上的 inode）。没有 store → 透传（临时文件可用；
  发布降级为 EXDEV）。
- 遗留清理：`tawcroot_linkstore_tmp_sweep()` 仅在顶层会话启动时运行，以 7 天为
  年龄门槛 — 会话模型不保证 guest 被拆除（被 init 收养的 exec-broker 后代会存活；
  存在并发会话；已经 spike 验证），因此急切清扫会在存活 guest 的脚下 unlink 它的
  临时文件。

## linkat 源检测（顺序很重要）

1. 模拟名字源在宿主尝试**之前**检测（符号链接属于 `lnk_file` 类；如果宿主策略
   允许 link 它们，就会把 token 符号链接本身硬链接 — 幽灵引用者）。
2. 位于 store 的源走 store 感知的、对每种写法都强制生效的兜底：解析器在 rootfs
   视图中捕获 token 名字；FOLLOW 模式的源还会被 O_PATH 打开，并将其
   `/proc/self/fd` 宿主路径与 `<store>/link/` 做前缀比较（→ ADD，token 从路径
   解析 — 对 `-k` 后缀 token 也有效）以及与 `<store>/tmp/` 比较（→ 发布）。这覆盖
   了 `/proc/self/fd/N` 写法，它们经由 /proc bind 路由，会跳过解析器。
   AT_EMPTY_PATH 源在宿主尝试**之前**用同样方式解析 fd 的宿主路径。NEW 绝不能
   rename 一个位于 store 的源 — 那会把对象本身 rename 出 `link/`，让整个簇悬空。
3. 否则先尝试宿主 linkat；EACCES/EPERM 触发模拟（对 AT_EMPTY_PATH 还包括
   ENOENT — 内核对无特权 CAP_DAC_READ_SEARCH 的拒绝）。目录源保留内核的 EPERM。
   视图内有名字的 AT_EMPTY_PATH 源 → 在 dev/ino 重新校验后经由
   `(AT_FDCWD, host_path)` 走 NEW；无名源（memfd、O_EXCL 临时文件、已完全
   unlink 的）→ 有意返回 EXDEV。
4. 符号链接的硬链接：NEW 把符号链接本身存为对象。解析器把符号链接对象的目标拼接
   回 guest 遍历中（`readlink_store` oracle 成员）— 相对目标相对于每个名字的父
   目录解析，绝对目标经由 guest 视图解析，与真实的硬链接符号链接完全一致。NOFOLLOW
   stat 报告对象真实的 S_IFLNK mode；readlink 转发对象的目标；普通的 O_NOFOLLOW
   打开仍然 ELOOP。

## 可接受的偏差（plans/ 之外、实际使用中观察到的）

- 对已打开对象的基于 fd 的 stat（fstat / AT_EMPTY_PATH）**确实**报告 sidecar
  nlink — 计划最初接受这里报告内核 nlink 1，但 GNU tar CREATE 根据它打开的 fd 的
  fstat 登记硬链接，这在归档时悄悄瓦解了硬链接结构（在 stage-5 tar 往返中发现）。
  开销门槛：只有位于 store 所在文件系统、内核 nlink 为 1、且其 inode 无法被共享
  对象集合（`include/linkobjs.h`）排除的 S_ISREG/S_ISLNK fd，才需要付出一次
  `/proc/self/fd` readlink。该集合由 `link_new` 在 rename 之前填充，并由对
  `link/` 的一次性扫描填充；它以 `link/` 的 inode 命名，因此复制后的 store 会重建
  它。
- 非 O_EXCL 的 O_TMPFILE：fstat nlink 为 1（而非 0）；`/proc/self/fd` 显示一个
  存活的 `tmp/` 路径（没有 " (deleted)"）。
- 模拟名字从不放在 bind 内：解析器有意跳过经 bind 路由的路径，因此放在那里的 token
  符号链接在 FOLLOW 打开时将无法解析（数据被困住）。linkat 改为降级 — 任一操作数
  位于 bind 上且宿主 link 被拒绝时，返回 EXDEV，不改变任何一个名字（工具会回退到
  复制）。rename/反向符号链接并不安全：Gradle 删除构建输出时也会丢掉其 NDK 库源。
  例外：直接位于 `/dev/shm` bind 中的两个名字会通过相对反向符号链接 rename，因为
  glibc sem_open 用 link(tmp, final) 发布一个已映射的文件，且没有 EXDEV 回退。
  目标位于 bind 的 ADD 也返回 EXDEV。bind 内的遗留 token 名字（来自该门槛之前）
  仍由内核追踪字面量 → ENOENT；它们的 d_type 会被改写，NOFOLLOW stat 也像 rootfs
  名字一样被修正。
- 在 store 存在之前启动的进程（LATENT）会在第一次**遇到** token 时打开它：
  FOLLOW 遍历经由解析器升级，linkat 在源检测之前升级，变更经由 `store_lock` 升级。
  剩下的缺口是仅 NOFOLLOW 的场景（处于陈旧 LATENT 状态的进程对模拟名字做 lstat，
  会报告为符号链接，直到上述任一情况触发）。
- 悬空的 token 名字（对象丢失：宿主复制不完整、NEW 崩溃窗口）在 NOFOLLOW 场景下
  表现为普通符号链接 — 可见且可 unlink 以便清理 — 在 FOLLOW/发布场景下为 ENOENT。
- 目录项中的 `d_ino` 是名字符号链接的 inode；位于 store 所在文件系统的目录（rootfs
  视图和同文件系统的 bind）中**每个**符号链接的 `d_type` 都被改写为
  DT_LNK→DT_UNKNOWN（迫使信任类型的遍历器去 stat，从而进入修正过的 handler）。
  目录按 `fstat` 设备分类；只有 procfs 列表需要付出 `/proc/self/fd` readlink
  （隐藏保留 fd）。
- 对模拟硬链接的 fd 路径自省（`readlink /proc/self/fd/N`、`/proc/self/maps`）会
  泄漏原始 store 路径（store 不在反向转换前缀之内）。`/proc/self/exe` 不受影响。
- 跨文件系统的 bind：不模拟（EXDEV）。
- 首次 link 期间有一个系统调用长度的旧名缺失窗口；崩溃窗口使计数偏高（从不偏低）；
  断电可能泄漏一个对象。

## 测试

`tawcroot/test.sh --host 'linkstore.*'`（保真度矩阵 + kill 矩阵，也在设备测试套件
中）。未配置 store 时 handler 会自行降级 — testhost/hosted 默认关闭，使 store
之前的测试继续覆盖 v1 兜底；linkstore 测试通过 `tawcroot_linkstore_configure`
显式启用。

已在模拟器上端到端验证（经由 exec broker 的 Debian sid rootfs）：ln 语义（含硬链接
的符号链接）、`cp -al` 农场 + `rm -rf` 源、硬链接目录树的 tar 往返（两个方向都保留
结构）、git 本地 clone（硬链接对象）+ gc + fsck、apt/dpkg 包安装、guest
`chroot(2)` 内的硬链接、6 进程 link/unlink 竞争压力测试（之后 store 干净：没有
intent、没有 `work/` 残留、簇已回收）、通过 magic-link 写法的 O_TMPFILE 发布及发布
后写入。性能抽查（模拟器）：对 3772 个条目执行 `find /usr/lib -type f` — 由于
DT_UNKNOWN 改写每个条目都被 lstat — 34 ms（每条约 9 µs，含陷入）；不算病态。
