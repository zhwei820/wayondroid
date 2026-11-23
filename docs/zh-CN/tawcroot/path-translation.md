# tawcroot — 路径翻译

## 路径翻译

这是价值所在的核心。对每个带路径参数的系统调用，我们：

1. 通过带防护的 guest 拷贝 helper，从 guest 地址空间读出路径字符串，
   长度上限为 `PATH_MAX`。
2. 判断是否需要翻译：
   - 位于 rootfs 视图内的绝对路径？→ 重写。
   - 匹配某个 bind mount 前缀的绝对路径？→ 重写到
     bind 源。
   - 相对路径？→ 相对内核 cwd（对
     `AT_FDCWD`）或传入的 `dirfd` 解析，把该基准反向翻译
     到 guest 视图，再在该 root/bind 视图内规范化。
     **不要**把相对路径原样透传。
3. 在每次调用独立的栈缓冲区中构造翻译并规范化后的路径。
4. 通过 stub 发出该系统调用的 *at 形式等价调用，
   以 rootfs fd 作为 `dirfd`、翻译后的后缀作为
   路径。几乎所有现代 Linux 系统调用都有 `*at` 形式；对于
   少数没有的（例如 `chroot`），我们先 open 再 fchdir/fchmod 等。
5. 返回该系统调用的返回值（正值或 `-errno`）。

我们不回写 tracee 的内存——系统调用是被
*模拟*的，而不是被转发。guest 永远看不到翻译后的
路径，只看到结果。这绕开了 proot 的一整类 bug：
"tracee 在参数读取与系统调用进入之间修改路径缓冲区"。

### 翻译规则

顺序很重要。先把 guest 请求归约为
guest 绝对路径：

- 若系统调用路径是绝对路径，原样使用。
- 若系统调用路径是相对路径且 `dirfd == AT_FDCWD`，调用原始
  `getcwd`，把返回的宿主路径反向翻译为 guest 路径，
  再拼接相对路径。内核 cwd 是唯一事实来源；
  MVP 中 tawcroot 不维护可变的 cwd 缓存。若 `getcwd`
  因 cwd 已被 unlink 而失败，目前返回与内核一致的错误；
  只有真实负载需要已删除 cwd 的兼容性时，才增加基于 fd 的 cwd 跟踪器。
- 若系统调用路径是相对路径且 `dirfd` 是 guest fd，通过
  `/proc/self/fd/<dirfd>`（或以后可能加入的
  轻量 fd 表）解析该 fd 的当前位置，把该
  宿主路径反向翻译为 guest 前缀，再拼接相对路径。若该
  fd 无法映射回 rootfs/bind 视图，返回 `-ENOENT` 或
  内核等价错误，而不是泄露宿主路径。

这种基于 fd 的相对解析需要 fd 来源信息，而不只是字符串
翻译。MVP 可以对普通目录 fd 使用 `/proc/self/fd/<dirfd>`，
但架构上必须为一张小型 fd 表预留空间，
记录 fd 来自 rootfs、bind 源、
`/proc`，还是 guest 视图之外。同一张表也用于
`fstat`、`fchown`、`fchmod`、`fchdir`、已删除目录的行为以及
内部 fd 保护。匿名 fd 和 guest 视图之外的 fd
不应被反向翻译为宿主路径。

然后对 guest 绝对路径 `P` 应用以下规则：

1. **`/proc` self 特殊处理。** 对我们自己的 pid
   （任意 tid，可带 `task/<tid>/` 段）的 `/proc/self/<x>` 和 `/proc/<pid>/<x>`，
   readlink 一侧：`exe` 由暂存的
   原始 guest 请求 exec 路径合成（见 §"`/proc/self/exe`"——
   经过我们的 exec 流程后，内核视角指向 libtawcroot.so）；
   `cwd` 通过与 `getcwd` 相同的
   最长前缀遍历反向翻译（视图外的 cwd → -ENOENT，
   同样不泄露宿主路径）；`root` 透传——我们从不
   做内核 chroot，所以内核给出的 "/" 已经符合
   guest 的预期，包括模拟 chroot 的情况。其他 `/proc`
   条目——以及其他进程的全部条目——原样透传
   （见 §"已接受的系统调用保真度偏差"）。

2. **bind mount 最长前缀匹配。** 若 `P` 以某个已绑定的
   `dst` 开头，重写为 `<src><P after dst>`，并通过 `*at` 形式
   从该 bind 预先打开的 fd 解析。

3. **默认 rootfs。** 其余情况：去掉开头的 `/`，通过 `*at` 形式
   相对 rootfs fd 解析。

4. **对绝对和相对请求都阻止 `..` 与符号链接逃逸。**
   不加干预时，内核的 `*at`
   解析会沿真实的磁盘目录走 `..`——因此像
   `/etc/../../host-secret` 这样的 guest 路径会逃出 rootfs
   视图，因为我们并没有真正 `chroot()`。相对路径，
   以及解析相对路径时遇到的绝对符号链接目标，
   也存在同样问题。

   我们通过 **handler 内路径解析**来阻止这种逃逸，但它必须
   按系统调用语义参数化。不存在一个对所有系统调用都正确的
   "规范化路径"行为：

   - **跟随末级**（不带 `O_NOFOLLOW` 的 `open`、`stat`、`access`、
     `chmod`、`chdir`、exec 目标）：解析包括
     末级在内的每个分量，通过 `readlinkat` 跟随符号链接，把
     绝对符号链接目标视为 guest 绝对路径，并把 `..` 钳制
     在所选 root/bind 的边界上。
   - **不跟随末级**（`lstat`、`readlink`、`open(O_NOFOLLOW)`、
     `fstatat(..., AT_SYMLINK_NOFOLLOW)`、`fchownat(...,
     AT_SYMLINK_NOFOLLOW)`）：只解析父级分量；末级
     分量作为叶子透传，不得被跟随。
   - **为创建解析父级**（`mkdir`、`mknod`、`symlink` 目标、
     文件不存在时的 `open(O_CREAT)`）：解析并钳制
     父目录，但不要求叶子存在，也
     不跟随不存在的叶子。
   - **为删除/重命名解析父级**（`unlink`、`rmdir`、`rename`
     目标）：解析父级，并保留叶子操作的
     内核语义（`unlink` 删除符号链接本身；`rmdir`
     要求是目录；`rename` 有自己的覆盖规则）。
   - **双路径操作**（`link`、`linkat`、`rename`、`renameat2`）：
     每个操作数按各自的模式解析。源和目标可以
     使用不同的 root/bind；只有在宿主系统调用或我们的兼容兜底
     无法模拟 guest 可见行为时，才拒绝跨 root 的情况。

   解析器返回所选的基准 fd、一个相对 root 的后缀，
   以及按模式决定的叶子处理方式。由系统调用 handler，而不是
   通用解析器，决定发出哪个宿主系统调用以及哪些标志。
   这样可以避免诸如跟随 `readlink` 的目标、解析
   `symlink(2)` 的源字符串、或把 `lstat` 变成
   `stat` 之类的 bug。

   **末尾斜杠语义叠加在这些模式之上。** 内核
   把末级分量之后的剩余字节（连续 `/`、`/.`）
   视为"跟随末级符号链接，并要求是目录"——即使在
   `O_NOFOLLOW`/`AT_SYMLINK_NOFOLLOW` 下也如此——而父级模式的操作保留
   原样的叶子，并把斜杠作用于它（`unlink("l/")` →
   ENOTDIR，`mkdir("l/")` → EEXIST）。词法折叠会抹掉这些
   字节，因此翻译层在原始 guest 字符串上检测它们
   （`has_trailing_dir_marker`，path_orchestrate.c），把
   叶子遍历从 NOFOLLOW 升级为 FOLLOW（绝对目标仍由解析器
   钳制），并在最终后缀上补回一个 `/`，
   让内核侧的系统调用以其原生 errno 形式强制执行目录
   要求。

   **普通 `openat` 上的 `AT_EMPTY_PATH` 不是可移植的捷径。**
   它直到内核 6.6 才加入（`openat2` 更早就有，但普通
   `openat(dirfd, "", AT_EMPTY_PATH, ...)` 在
   更早的内核上返回 `-ENOENT`）。当翻译后的后缀为空（guest 请求
   的是 "/"）时，改传 `"."`——它解析到 `dirfd`
   所指的目录，在我们面向的所有内核上都可用。对 `chdir`，后缀为空时
   直接 `fchdir(base_fd)`。对
   `*statat`/`readlinkat` 等，该标志支持良好，可以使用。
   （已在 Android 16 / 内核 6.6 模拟器上实测确认。）

   **不使用 `openat2(RESOLVE_IN_ROOT)` 捷径。** 早期设计
   曾在内核 ≥ 5.6 时于 `handle_openat` 中有条件地使用带 `RESOLVE_IN_ROOT` 的 `openat2`，
   让内核把绝对符号链接目标重新以 `base_fd` 为根。
   这破坏了跨 bind 的绝对符号链接：当 `base_fd` 是某个 bind 源目录 fd 时，
   绝对符号链接目标（例如 Android 的 `/system/lib64/libc.so
   → /apex/com.android.runtime/lib64/bionic/libc.so`）会被
   重新以 bind 源而不是宿主根为根，于是
   打开的是 `<bind_src>/apex/...`，以 `ENOENT` 失败，
   guest 看到 "library not found"。具体来说：这悄无声息地
   破坏了 Pixel 10 Pro Fold（内核
   ≥ 5.6）上 libhybris 的 bionic-libc 加载，而 Pixel 4a 这类旧设备（内核 4.14，没有
   `openat2`）正常工作。现在手工解析器是唯一的
   契约，与内核版本无关，`handle_openat`
   始终使用普通的 `tawc_openat`。回归测试：
   `tests/integration/test_prod_rootfs.c` 中的
   `prod_rootfs_cross_bind_abs_symlink`。

   这不是安全边界——见 §"What it explicitly is
   not"——但它能阻止意外逃逸（构建脚本、配置
   文件、使用相对路径的 tarball 解包）悄无声息地
   触达宿主内容，即使对可信 guest 这也是*正确性*问题。
   proot 做了同样的规范化加钳制；
   我们希望功能对齐。

   **感知符号链接的规范化开销。** 现代 glibc rootfs
   中 `/lib`、`/lib64`、`/bin`、`/sbin` 是符号链接（通常
   指向 `/usr` 内）；ld.so 在每次程序启动时都经由它们打开库。
   朴素实现下每次 `openat` 都要沿符号链接链做若干次
   `readlinkat`。**在初始化时缓存常用目录路径的
   规范形式**（`/lib`、`/lib64`、`/usr/lib`、`/bin`、`/sbin`、`/etc` 是
   典型命中集），使每次系统调用的规范化开销
   在热路径上接近零。这一缓存对
   性能至关重要——不是可选项。该缓存没有
   失效机制——若会话中途 rootfs 的符号链接发生变化（例如
   `pacman` 替换了 `/lib`），陈旧条目会悄无声息地
   把 open 导向错误位置。目前可以接受：这些符号链接在 ALARM 中
   实际上不可变。若出了问题，就在对已缓存前缀做
   `symlinkat`/`rename`/`unlinkat` 时使其失效。

### 共享路径缓存

解析器用 `readlinkat` 探测每个前缀，每个分量一次系统调用，
在 Android 上这比 guest 请求的大多数系统调用都贵。
生产 oracle（`prod_readlink`，path.c）先
查询 `include/pathcache.h`：一个位于
link store 旁边（`<rootfs>/../tawcroot-pathcache`）的发行版级 `MAP_SHARED` 文件，每个
前缀保存一条事实："readlinkat 返回了 `-EINVAL`"（存在，且不是符号链接）。

- **键**对规范宿主路径（rootfs 宿主路径 + `/` +
  后缀）做哈希，因此 guest chroot 对相同对象共享键。bind
  路由从不缓存。
- **失效。** unlink/rmdir/rename（两个操作数）在宿主调用之后
  用新的 epoch 给该名字打戳；一个条目只有在
  其 epoch 不小于其自身各前缀上最新的戳时才有效。条目携带
  在其 `readlinkat` 之前读到的 epoch，因此
  在某次修改完成之前观察到的事实不会比该修改活得更久。linkat
  （可以通过多种途径把其源变成模拟硬链接符号链接）
  会清空整个缓存，每次顶层 tawcroot
  启动也会清空——用于防范会话之间在 tawcroot 之外做出的修改。
  会话运行期间在 tawcroot 之外做出的修改
  不会被察觉。
- **并发。** 跨进程的逐槽 seqlock，无锁；发生争用的插入
  直接跳过。每个键使用两个独立的 64 位哈希。
- 创建或映射该文件的任何失败都会让缓存处于关闭状态。
- **增量键。** 在一次解析器遍历中，oracle 看到的是
  同一缓冲区不断增长的前缀（`path_oracle.h` 中的 `walk_begin`
  标记每次遍历），因此生产 oracle 把上一次
  探测的长度和哈希状态向前传递（`tawcroot_pathcache_walk_*`），
  而不是对整个前缀重新执行 `strlen` 和哈希。
  对每个前缀重新哈希是路径长度的 O(n²)，也是 tawcroot 中最大的
  用户态开销（在 find/ls/tar
  profile 中占其样本的 21%）。前缀戳在首次哈希时读取，因此与该遍历
  竞争的修改在本次遍历剩余部分中可能不被察觉，这与原生
  遍历和 rename 竞争的情况相同。

单元测试覆盖：`tests/unit/test_pathcache.c`。

### fd 路径缓存

每个相对 dirfd 的路径都通过该
dirfd 的 `/proc/self/fd` 链接提升为 guest 绝对路径（`translate_local`）；`find`/`tar`/`grep`
会发出成千上万次。`include/fdpath.h` 按进程缓存 fd → guest 路径，
并在每次使用时重新校验：`fstat` 必须仍报告缓存的
dev/ino（被关闭后复用的 fd 号、被 `dup2` 覆盖、或 vfork
子进程已分叉的 fd 表，都不会拿到陈旧路径），共享路径
缓存必须报告自填充以来该路径及其任何祖先都没有被修改
（fd 保持打开期间的 rename/rmdir），且 root 视图必须
未变。只缓存普通 rootfs 路径（路径缓存不给
bind 名字打戳），路径缓存关闭时什么都不缓存。测试：
`tests/hosted/test_fdpath.c`。

**叶子捷径。** 提升后的目录路径就是 dirfd 的真实
位置，因此不含符号链接分量。对于读意图、
`FOLLOW`/`NOFOLLOW`、且是位于所有 bind 之外的单个普通名字（不含 `/`，不是 `.`/`..`）
的情况，`dirfd_leaf_shortcut` 交给内核的是
`(dirfd, name)` 而不是 `(rootfs_fd, whole/path/name)`：没有对每个前缀的用户态
遍历，也没有内核从根重新遍历。`FOLLOW`
还需要路径缓存确认叶子不是符号链接；写操作保留
基于 root 的路由，以便其修改给路径缓存打戳。

### DAC override

tawcroot 伪装 uid 0，但内核在每个真实
系统调用上看到的都是 app uid，且 `untrusted_app` 从不持有 CAP_DAC_OVERRIDE。因此一个
拒绝**属主**的权限模式也会拒绝 guest 的 "root"：当
`/` 为 0555 时，`mkdir /test` 返回 EACCES（upstream #12），`[ -w / ]`
回答否。除此之外别无问题——`chown` 需要我们从来没有的 CAP_CHOWN，
所以整个 rootfs 都归 app 所有，权限模式是唯一的杠杆；而
`chmod` 只需要所有权，所以 guest root 可以自己设置限制性模式，
只有*下一次*写入才失败。proot 的 `-0` 主动地模拟了该
capability（`fake_id0` 的 HOST_PATH hook 在系统调用之前把路径的每个
分量 chmod 为 u+rw，目录再加 u+x，并在系统调用退出时
恢复），所以这是相对 proot
安装方式的回归，不是发行版的特性。

我们用同样的方式模拟它，但只在错误路径上——正常
路径不能承担 proot 那种每次系统调用都做的 stat 遍历。

- **在分发调用点放一个包装器**（`rescue.c`，
  `tawcroot_dispatch_call`），而不是每个 handler 各自重试：unlinkat、
  renameat2、linkat 和 openat 是多步操作，"重试原始
  系统调用"不适用于它们，而且 EACCES 可能来自 handler 从未暴露的路由上的
  link-store 操作。取而代之的是，
  翻译器把它产生的每条路由（`base_fd` + 后缀）放进
  包装器拥有的逐线程 rescue 槽，方式是
  在 `tawcroot_path_translate` 的唯一出口以及 `translate_local` 中那个 dirfd 透传分支处调用
  `tawcroot_rescue_note()`。新的路径 handler 无需改动
  `rescue.c` 即被覆盖。
- **放宽什么。** 每条路由的基准 fd（通过
  `fchmodat(fd, ".")`——基准 fd 是 O_PATH，`fchmod` 对其返回 EBADF），
  然后是路径的每个前缀。只处理 app 拥有的 inode（`st_uid` == 真实
  uid）：缺少 `u+rwx` 的目录获得它们；叶子只有在
  是普通文件**且**该系统调用确实涉及
  叶子自身权限时（open、truncate、access、chdir、chroot、
  exec、utimensat、基于路径的 xattr 调用——见
  `rescue_needs_leaf` 中的表）才获得 `u+rw`。unlink、mkdir、mknod、symlink、link 和 rename
  只需要父级；放宽它们的叶子是白费功夫，
  而且按路径的恢复会落空，因为名字已经消失或被移走。
- **绝不透过符号链接 chmod。** 在 NOFOLLOW 操作下叶子是符号链接，
  每个模拟硬链接名都是
  `tawcroot:link:<token>` 符号链接，而 `fchmodat` 没有
  `AT_SYMLINK_NOFOLLOW`。遍历以 `AT_SYMLINK_NOFOLLOW` 做 stat，
  不碰任何不是目录或普通文件的东西。
- **绝不给文件加 x。** 这是 CAP_DAC_OVERRIDE 自身的规则：
  root 不能执行没有执行位的文件，且对这样的文件做 `access(X_OK)`
  对 root 也返回 EACCES。exec handler 的"无 x 位 → EACCES"
  检查（`exec_handler.c`）保留。
- **只有在遍历发现了 app 拥有的分量时才重试**——而不是"只有
  确实放宽了什么时"。并发的 rescuer 可能已经放宽了
  同一个目录，跳过重跑会把这种竞争
  变成一次虚假的 EACCES。我们无法拥有的拒绝（SELinux、指向
  `/dev` 或 `/sdcard` 的 bind）什么也找不到，直接返回原始 EACCES，
  不再运行第二次 handler。
- **恢复，而不是永久放宽**，从最深处开始（若先恢复
  失去搜索位的父目录再恢复子项，子项会被困住）。
  guest 始终看到它自己设置的模式，因此 `pacman -Qkk`、
  sudo 对 0440 sudoers 的检查以及 ssh StrictModes 都保持干净。在放宽与恢复之间
  崩溃会留下更宽的模式；无害。
- **以虚拟 euid 0 为前提**，只在 EACCES 路径上读取。已经
  降权的 guest 得到真实的 EACCES，与下文的
  fchmodat/fchownat 门控一致——其他所有拦截都不读取
  identity seqlock。
- **记录是惰性的。** 首次运行不记录任何东西；只有
  在虚拟 root 下出现 EACCES 时才占用一个槽，并重跑一次 handler 来
  记录路由，然后放宽并重试。
- **exec 从不由包装器重试**：重试会重复
  终结性的 re-exec。`exit(2)` 不被拦截（guest 可能已经
  unmap 了它的栈）。取而代之，`handle_execve` 的探测和 shebang
  打开，以及 `--exec-child` 加载器对 PT_INTERP / 解释器的
  重新打开，使用 `tawcroot_rescue_open_in_view()`，它围绕一次 open 做放宽、
  打开和恢复。fd 保留其访问权限，因此一个
  `--x--x--x` 的二进制（4111 的 sudo）在 re-exec 的另一侧通过其固定的 fd
  加载。
- **重跑一个中途遇到 EACCES 的 handler 是安全的。**
  link-store 修改是仅有的多系统调用操作；`linkstore.c` 中每条错误
  路径在返回前都会清空 intent 槽并解锁，且设计是
  单调的（count >= 存活引用者数），因此
  一次重放最坏只造成 +1 的多计——这在
  docs/tawcroot/link-emulation.md 中已说明是安全的。

固定上限，全部退化为"不救援"（救援机制出现之前的行为），
而不是给出错误答案：64 个可同时救援的线程，每次调用 2
条路由（linkat/renameat 翻译两个路径；没有调用
翻译更多），每条路由 1024 字节，每次调用最多放宽 40 个 inode。

已知限制：

- **没有属主搜索位。** 符号链接遍历器会传播来自
  `readlinkat` 的 EACCES（`path_resolve.c`），因此 app 无法
  *搜索*的目录会在翻译过程中失败，此时完整路由尚未形成——
  无可救援。有 x 而无 w 的目录（`/` 的情况，以及
  ALARM bootstrap 的 0500 `/etc/ca-certificates/extracted/cadir`）
  是实践中仅见的情形。
- **跨父目录重命名目录**需要对该目录
  自身有 `w`（更新 `..`）。rename 不放宽叶子，因此 u-w
  的目录仍无法移动。实践中未见；修复它意味着
  在目标路由上恢复。
- **竞争**，与 proot 相同：两个进程救援同一
  目录时仍可能让其中一个得到虚假的 EACCES（第一个在
  第二个重跑之前就恢复了），恢复可能覆盖对同一 inode 的并发
  guest chmod，且其他线程会短暂看到
  放宽后的模式。可以接受——rootfs 是同一个安全主体
  （docs/tawcroot/overview.md）。
- **只有组/其他执行位。** 对只有组或其他 x 位的文件，
  root 的 `X_OK`/exec 仍是 EACCES：对 app uid 起决定作用的是
  属主位。
- 多线程 guest 的 **fork** 会让任何兄弟线程
  正在使用的槽在子进程中保持占用（clone 不被拦截，所以
  fork 从不发生在包装器内部）。有界，在 tid
  复用时自愈，表满也只意味着不救援。

被否决的替代方案：在 chmod/mkdir/open 时钳制模式，使
目录树永不持有限制性的属主模式。它改变了 guest
看到的东西，不能修复磁盘上已有的模式，不覆盖如实的
0500 cadir，且偏离了 proot 的语义。

### 哪些系统调用需要拦截

覆盖我们用途的最小集合。系统调用号与架构相关；
分发表在构建时由系统调用列表生成。

**虚拟身份与元数据（有状态，类似 proot 的 `fake_id0`）：**

身份是一个被跟踪的进程级结构体（`identity.c`：
`tawc_identity`——r/e/s/fs uid+gid，外加 32 项的附加组
影子），初始化为 root（全为 0，组为 `{0}`），由
seqlock 保护（signal-shadow 模式：固定大小状态 + 原子序列
计数器；通过 CAS 占用实现多写者安全）。它的存在是为了让那些
降权并*验证降权*的守护进程（OpenSSH `permanently_set_uid`）
行为正确：降权后，恢复旧 id 返回 EPERM，getter 报告
目标用户。各处的特权判定都是虚拟
`euid == 0`——伪 root 拥有全部 capability，其他人一个都没有。

- `getuid`、`geteuid`、`getgid`、`getegid`、`getresuid`、`getresgid`、
  `getgroups` → 报告跟踪的值。拦截 `getgroups` 是为了不让
  内核把 app 的 Android gid（3003/9997/…）泄露到 `id` 中。
- `setuid`、`setgid`、`setreuid`、`setregid`、`setresuid`、
  `setresgid`、`setfsuid`、`setfsgid`、`setgroups` → 把 Linux 语义
  应用到虚拟状态（包括 setreuid 的 saved-id 更新
  规则以及 setfsuid 的"返回旧值、从不报错"契约）。
  超出 32 项影子的 `setgroups` 返回内核上说得通的
  `-ENOMEM`。没有 `seteuid` 系统调用；libc 经由 setres*id 实现。
- `fork` 自动继承身份（地址空间拷贝）；`execve`
  通过 exec_state（v4，`has_identity` + 内嵌
  结构体）携带它，在 supervisor 初始化后于 `--exec-child` 中恢复。
- `stat`、`lstat`、`fstat`、`fstatat`、`newfstatat`、`statx` →
  宿主系统调用成功后，把结果拷贝到一个本地
  结构体，对 rootfs 拥有的路径/fd 把 uid/gid 字段改写为 0，并
  通过 `copy_to_guest` 把修饰后的结果拷回 guest。
  宿主错误原样透传。修饰**不**感知
  身份（降权后的进程仍看到 root 拥有的文件，
  仍能打开 app uid 能打开的任何东西）——与 proot 的偏差
  相同；强制执行不是目标。
- `/proc/self/status` **不**合成：其 `Uid:`/`Gid:`/`Groups:`
  行显示真实的 app id，与 getter 不一致。已知
  偏差；我们面向的程序都不解析它来获取身份。
- `chown`、`lchown`、`fchown`、`fchownat` → 翻译路径/fd，
  当虚拟 euid == 0 时，校验存在性后返回成功，
  不做真实的宿主 `chown`（app uid 不能 chown；磁盘上的
  rootfs 有意保持为 app 所有）。降权时，转发
  真实的宿主系统调用，使真实的权限错误得以暴露。
- `chmod`、`fchmodat` → 翻译并**尝试**宿主 chmod（模式
  在 rootfs 内是有意义的，通常会真实生效——app uid
  拥有这些文件），但当虚拟 euid == 0 时把宿主的
  `EPERM`/`EACCES` 吞掉变为成功：root 不会从 chmod 得到权限错误。
  与下文 `mknodat` 设备节点降级
  以及上文惰性 DAC override 的道理相同：凡是 app uid 做不到
  root 能做的事，我们要么做最接近的真实操作，要么偏向
  root 撒谎——绝不暴露一个 guest 的
  身份认为不可能出现的特权错误。具体使用者：sshd 的 `pty_setowner()` 对
  `/dev/pts/N` 的 chmod，Android SELinux 拒绝它，而 sshd 对
  每次 TTY 登录都视之为致命错误。非权限错误透传；降权
  进程得到真实结果。基于 fd 的 `fchmod` 也被拦截，用于
  相同的吞错契约以及只读 bind 的阶段 2 检查（见
  §"只读 bind"）。
- 身份无关修饰的一个值得了解的后果：降权
  之后，guest 自己创建的文件 stat 结果仍是 uid 0，因此
  降权会话中的 `test -O` / 所有权检查会误报。
- 不在范围内：没有特权*强制执行*（身份只是
  表面一致），exec 时不遵循 setuid 位（降权
  进程不能通过 `su`/`sudo` 重新提权，与 proot 相同），
  除 euid 判定外不模拟 capget/capset，不感知按用户的
  NSS（id 只是数字）。`chroot` 和 `mknod` 保持
  无门控（内核会要求 CAP_SYS_CHROOT/CAP_MKNOD）：
  降权的 guest 仍可虚拟 chroot / 尝试 mknod。sshd 的
  privsep chroot 发生在其降权*之前*，所以不需要门控；
  若有负载依赖该拒绝再重新考虑。
- 已知的有界失败（与 chroot 全局变量 /
  signal shadow 的立场相同）：vfork 子进程在 exec 前调用 set*id 会破坏
  父进程的视图；guest 信号 handler 中的 set*id 若
  打断了同一线程上的另一次 set*id，会在写者占用上自我死锁；
  在这样的 handler 中做身份*读取*——
  get*id，或任何查询 `tawcroot_identity_euid()` 的 handler
  （chmod/chown 特权门控）——会在奇数的 seqlock 上永远自旋；
  在另一个线程处于 set*id 中途时 fork，会让子进程的 seqlock 保持
  奇数。所有这些都要求 guest 信号或 fork 恰好落在 set*id
  几十条指令的写窗口内；没有已知负载会这样做。

**运行时控制面（保护 tawcroot 自身的不变量）：**

- `close`、`close_range`、`dup`、`dup2`、`dup3`、`fcntl` → 隐藏并
  保护 tawcroot 保留的内部 fd 区间；guest 可见的 fd
  行为正常。`close_range` 基于 `/proc/self/fd` 模拟，
  而不是重新发出（NR 436 会被 API 34 之前的策略拦截）。
- `rt_sigaction`、`rt_sigprocmask` → 虚拟化 `SIGSYS`，使 guest
  无法移除或屏蔽 tawcroot 真实的拦截 handler。
- `sigaltstack` → 替换过小的 guest altstack（我们的 SIGSYS
  handler 是 `SA_ONSTACK`）。
- `seccomp`、`prctl(PR_SET_SECCOMP)`、相关的 `prctl` seccomp 查询
  → MVP 中拒绝 guest 安装过滤器；只暴露兼容的
  查询行为。

**带路径参数（重写路径参数，切换到 `*at` 形式）：**

- `open` → `openat(AT_FDCWD, …)` 本身已是 at 形式；内核的 `open`
  不接受 dirfd，所以我们用 rootfs fd 经由 `openat`
  路由。
- `openat` → 规范化之后用所选 root/bind fd 重新 `openat`。
  绝对路径以 guest `/` 为根；
  相对路径先对反向翻译后的
  内核 cwd 或传入的 guest dirfd 解析，再钳制在
  同一 root/bind 视图内。
- `openat2`（内核 ≥ 5.6）——同上。
- `stat`、`lstat`、`fstatat`、`newfstatat`、`statx`。
- `access`、`faccessat`、`faccessat2`。
- `chmod`、`fchmodat`。
- `chown`、`lchown`、`fchownat`。
- `mkdir`、`mkdirat`。
- `rmdir`、`unlink`、`unlinkat`。
- `symlink`、`symlinkat`。
- `link`、`linkat`——翻译两个路径。若宿主 `linkat`
  在 Android app-data SELinux 策略下以 `EACCES`/`EPERM` 失败，
  则通过树外的 link store 模拟**完整**硬链接：
  文件数据移到 `distros/<id>/tawcroot/link/<token>`，每个
  guest 名字都变成不透明的 `tawcroot:link:<token>` 符号链接，由
  解析器和 NOFOLLOW handler 映射回该对象——共享
  inode、实时的 `st_nlink`、任意顺序 unlink、崩溃安全的计数
  修改。store 格式、源检测顺序、O_TMPFILE 发布以及可接受的
  偏差见 [link-emulation.md](link-emulation.md)。目录源会做 stat 检查并保留
  内核自身的 EPERM（绝不把目录 rename 走）。在 store
  无法存在的地方（无 store 路径、跨文件系统的 bind 源），store 之前的 v1
  兜底仍然适用：`renameat2(src → dst, RENAME_NOREPLACE)`
  加上旧名字处的 guest 绝对符号链接——方向很重要
  （git 通过 `link(tmp, final); unlink(tmp)` 发布；放在
  `final` 处的符号链接会在 unlink 后悬空）。v1 覆盖：
  `test_hook_faults.c` 中的 `hosted_linkat_fallback_*`；store 覆盖：
  `tests/hosted/test_linkstore*.c`。
- `rename`、`renameat`、`renameat2`。
- `readlink`、`readlinkat`。
- `truncate`。
- `chdir`——翻译，打开目标目录，然后 `fchdir`，使
  内核的 CWD 与之一致。我们有意不更新单独的
  guest-cwd 全局变量；之后的相对 `AT_FDCWD` 路径和 `getcwd` 都
  从内核 cwd 推导并对其做反向翻译。
- `getcwd`——调用宿主，然后反向翻译（去掉
  rootfs 前缀，前置 `/`，或查找 bind 源并输出
  其绑定目标）。
- `chroot`——guest 发出的 chroot 比较有意思。proot 拒绝它
  或重新绑定。我们拒绝（`-EPERM`，`syscalls_control.c` 中的
  `fake_eperm`），若 guest 确实需要在 chroot 内部再
  chroot 再重新考虑。
- `pivot_root`、`mount`、`umount2`、`unshare`、`setns`——同样的
  `fake_eperm` 拒绝。拦截它们是为了不让其打乱我们的路径
  记录或拆掉启动时建立的 bind。
- `execve`、`execveat`——路径参数需要翻译，并且
  ELF 内的加载器/解释器路径（`PT_INTERP`、
  `/lib64/ld-linux-x86-64.so.2`）需要正确解析。
  一旦我们翻译了二进制自身的路径，内核会自己完成这一步，因为
  内核在 `execve` 期间发出的内部 `openat` 调用会经过……等等，不对，
  这些是内核在内核态自行发出的，不经过我们的 seccomp
  过滤器。这意味着若宿主上不存在 `/lib64/ld-linux-x86-64.so.2`，
  exec 就会失败。因此我们必须确保
  ELF 的 `PT_INTERP` 路径能被解析。两种方案：
   - **bind mount 方案**（proot 在某些平台上这样做）：
     在宿主中预先 bind `/lib64`、`/lib`、`/usr/lib` 等，使
     内核能找到解释器。很别扭——会把 rootfs 路径泄露
     到宿主目录树中。
   - **加载器注入**（proot 首选的方案）：重写
     `execve`，运行我们自己的加载器 stub，再以
     `LD_TRACE_LOADED_OBJECTS` 风格的间接方式 re-exec……不，更简单：
     读取 ELF，找到 `PT_INTERP`，经由
     rootfs 路径下的解释器手工 `mmap`+`execve`。proot 的加载器正是
     这样做的。我们实现同样的思路，但更干净。

  计划：读取 ELF 头，提取 `PT_INTERP`，翻译它，然后
  以 `--exec-child` 方式 re-exec tawcroot。子进程重新安装 handler，
  打开翻译后的二进制/解释器，`mmap` 它们的 PT_LOAD
  段，合成初始栈/auxv，并跳转到 ld.so 的
  入口点，而不做第二次 `execve`。见下文"execve 处理细节"。

- `mknod`、`mknodat`、`statfs`——拦截、翻译、分发。
  `mknod`/`mknodat` 路由到现代的 `mknodat`，作用于
  `(base_fd, suffix)`；FIFO/socket 节点在 tmpfs 上成功。字符/
  块设备在生产环境中以 `-EPERM`/`-EACCES` 失败（Android
  `untrusted_app` 没有 CAP_MKNOD）；在虚拟 euid 0 下，
  handler 把被拒绝的设备 mknod 降级为一个空的普通文件
  占位（proot 的 fake_id0 行为——postinst/makedev
  脚本得以继续，打开伪节点时才在使用时失败），若
  连占位都被拒绝（guest 的 `/dev` 是宿主 `/dev` 的
  bind，app 在其中什么都不能创建），则吞掉错误返回单纯的
  成功，与 fchmodat/fchownat 的契约一致。EEXIST 和
  其他非权限错误如实返回；真正降权的
  身份看到真实错误。`statfs` 经由
  `/proc/self/fd/<base_fd>/<suffix>` 分发（没有 `*at` 形式，且在
  Android 出厂的 5.4 内核上对 `O_PATH` fd 做 `fstatfs` 不可靠）。
  `fstatfs`（基于 fd）保持不拦截。

- `setxattr`/`getxattr`/`listxattr`/`removexattr` 以及 `l*`
  符号链接 NOFOLLOW 变体——拦截、翻译，经由
  `/proc/self/fd/<base_fd>/<suffix>` 分发。基于 fd 的*写者*
  （`fsetxattr`/`fremovexattr`）只为
  只读 bind 阶段 2 检查而拦截（§"只读 bind"），其余情况
  原样透传；基于 fd 的读者保持不拦截——内核
  对已打开 fd 的解析本来就是正确的。在 Android app 私有存储上大多数调用
  返回 `-EOPNOTSUPP`；拦截的价值
  在于失败是针对 guest 可见路径的，
  而不是宿主相对路径。

- AF_UNIX sockaddr 翻译（`syscalls_socket.c`）：`bind`/
  `connect`/`sendto`/`sendmsg` 对路径名形式的
  `sun_path` 做正向翻译；`getsockname`/`getpeername`/`accept`/`accept4`
  把输出参数中的地址反向翻译回 guest 视图。
  `recvmsg` 的 `msg_name` / `recvfrom` 的 `src_addr` 数据报源
  地址**有意不**做反向翻译：`msg_name`
  位于 guest 的 msghdr 内，因此 BPF 过滤器无法有条件地
  拦截，而 `recvmsg` 是系统中最热的接收系统调用
  （每条 Wayland/X11/dbus 消息）——拦截它会给
  所有 guest 接收流量加税，只为了覆盖一个没有已知使用者的情况。
  数据报源地址只有在发送方
  显式 `bind()` 了路径名 socket 时才携带路径；glibc `syslog(3)` 和
  sd_notify 都不这样做。若有负载做基于路径寻址的
  数据报请求/应答再重新考虑（症状：应答消失，因为
  宿主路径形式的源地址在
  经由 `sendto` 发回时又被正向翻译了一次）。

  `sun_path` 为 108 字节（含 NUL），而生产安装
  前缀（约 45+ 字节，发行版 id 长时更长）会消耗在每个
  翻译后的路径上，因此正向渲染分层：(1) 宿主绝对路径
  `<prefix>/<suffix>`——优先，因为有一种设备组合在 app
  上下文中对经由 `/proc/self/fd/N/...` 的 AF_UNIX bind/connect 返回了
  `ENOENT`（pacman-key 修复）；(2) 溢出时，
  `/proc/self/fd/<base_fd>/<suffix>`（长期存在的 rootfs/bind fd，无
  频繁开关）；(3) 当后缀本身很长时，
  `/proc/self/fd/<parent_fd>/<leaf>`，使用叶子
  父目录的 O_PATH fd。内核逐字保存 bind 的 sockaddr 字节，并
  在 getsockname/getpeername/accept 中原样回显，因此 `bind` *保留*它的
  第 3 层父目录 fd，并记录在一张按 (dev,ino) 去重的小表中
  （`sock_parents`，16 项）；反向翻译通过记录的 fd（readlink）或
  rootfs/bind 宿主前缀展开 `/proc/self/fd/<n>/...`，
  然后再做常规的最长前缀遍历。
  connect/sendto/sendmsg 在调用后立即关闭其第 3 层 fd——
  它们的字符串从不被回显。降级：表
  满/保留失败 → bind 仍可工作，但其 getsockname 显示
  原始的 /proc 写法；保留的 fd 是 CLOEXEC，因此在
  execve 之后，陈旧写法原样透传（表为空），
  而不是通过被复用的 fd 号解析。若 app 上下文中
  `/proc` bind/connect 的怪癖再次出现，超出预算的路径会在那里失败
  （ENOENT，而不是旧的 ENAMETOOLONG）——所有在预算内的路径
  都留在第 1 层；若真实负载碰到它，再用 supervisor 辅助的 bind 重新考虑。
  2026-07 在实体设备上以 app
  上下文验证（arch rootfs）：第 2/3 层 bind/connect/getsockname/getpeername
  往返以及超预算的数据报 sendto 均可工作——
  pacman-key 的 ENOENT 怪癖在那里不影响这些 `/proc` 写法。

- `socket`（`syscalls_socket.c`）：`AF_NETLINK` + `NETLINK_AUDIT`
  返回 `-EPROTONOSUPPORT`——这是没有 `CONFIG_AUDIT` 的内核
  会给出的答案（netlink 族存在，协议未注册）。Android
  SELinux 以 `EACCES` 拒绝 audit socket，而 libaudit
  的使用者（Debian libpam、sshd 的 linux-audit 支持）会把它升级为
  硬失败：`pam_acct_mgmt` → `PAM_SYSTEM_ERR`（破坏整个 rootfs 的
  `su`/`login`/带 PAM 的 sshd），且 sshd 在 TTY
  登录时 `fatal()`。其他所有族/协议透传。

- `mount`、`umount`——翻译路径。大多数会在
  内核层失败；这没关系。

- `io_uring_setup`——返回 `-ENOSYS`。这不是我们
  推迟的功能；不拦截 io_uring 会让 guest 提交
  带路径的 SQE（内核从 app 共享
  内存读取 SQE，而不是经由我们过滤器能看到的系统调用），这些 SQE 作用于
  *宿主*文件。拒绝 setup 会让程序回退到
  基于系统调用的 I/O，而那是我们会翻译的。完整拦截
  的设想见 §"Open questions"。

**非路径相关的怪癖（Android 叠加的过滤器）：**

lp64 上的 `untrusted_app` 过滤器对那些在
某些架构上存在而在另一些架构上不存在的系统调用做 RET_TRAP（这就是
`notes/proot.md` §"Why upstream proot doesn't work on Android
x86_64" 中记录的问题）。具体在 x86_64 上：`access`、`open`、`chmod`、`chown`、
`mkdir`、`rmdir`、`unlink`、`symlink`、`link`、`rename`。我们在自己的
过滤器中也 TRAP 这些，并且 handler 总是重写为
`*at` 等价形式——这意味着作为做路径翻译的
副作用，我们悄无声息地修补了 Android 的缺口。这是 proot 的
`src/tracee/seccomp.c` 所带那段权宜之计最干净的版本。

在 aarch64 上，所列系统调用本来就没有独立的调用号——
glibc 原生经由 `*at` 路由——因此
Android 过滤器这一侧在那里是空操作，但我们仍在
自己的过滤器中拦截它们以防万一（以防静态二进制
直接发出它们）。

**为什么我们拦截的系统调用这么少：**

seccomp 过滤器应尽可能窄。每次 TRAP 都是一次
SIGSYS 往返，而现代程序会发出*大量* `read`、
`write`、`futex`、`epoll_wait`、`mmap`、`clock_gettime` 调用。这些
都不需要翻译。TRAP 集合应严格限定为：

- 上面的带路径参数列表。
- `getcwd`（因为必须做反向翻译）。
- 虚拟身份/元数据系统调用（`getuid`、`set*id`、`stat`、
  `chown` 等）——proot `-0`/`fake_id0` 的有状态对应物。
- 维护 tawcroot 不变量所需的运行时控制系统调用
  （用于内部 fd 保护的 `close*`/`dup*`/`fcntl`，
  用于 `SIGSYS` 的 `rt_sigaction`/`rt_sigprocmask`，以及
  用于对 guest 过滤器假装接受的 `seccomp`/`prctl`）。
- `io_uring_setup`，使带路径的 SQE 无法绕过翻译。
- Android 叠加过滤器集合中我们想悄无声息地重写的那些。

这仍是约 350 个系统调用中的一个小子集。其余保持
`RET_ALLOW`，包括所有热点调用。非路径系统调用的每次调用开销：
零（只有内核执行 BPF，
是纳秒级）。不存在跳过 RET_ALLOW 上 BPF 执行的内核开关；
每个系统调用这一纳秒不可避免，且远
低于 proot 的 ptrace 往返。

**按 fd / 按参数的 BPF 快速路径。** 有些被拦截的系统调用只对
其参数值的一小部分做实际工作。seccomp
过滤器可以对无关的调用 `RET_ALLOW`，使它们永不进入
handler。目前只有 `close` 有一条（对 `args[0]`
与 `TAWCROOT_RESERVED_FD_BASE` 做区间比较；为修复 gpgme/`closefrom`
死亡螺旋而加入，见下文历史）。同样的形式适用于其他调用：

- `fcntl`：只有当 arg0（fd）不低于
  `TAWCROOT_RESERVED_FD_BASE` 时才需要处理——包括 `F_DUPFD`/`F_DUPFD_CLOEXEC`，
  因为 handler 不再限制请求的最小值。目前
  `syscalls_fd.c` 中的 handler 已经正确编码了这一区分；
  BPF 还没有，所以 glibc 的 TLS/加锁、GnuTLS 的
  fd 标志清理以及 stdlib 热路径都会在
  `F_GETFD`/`F_SETFD` 上不必要地被拦截。BPF 块的形式与
  `close` 的相同：对 `args[0]` 做一次区间比较。

这些都是纯性能优化——当某个负载在相应系统调用上
可测量地受 handler 限制时再加（在压力
运行下按系统调用号统计 `tawcroot_dispatch_*` 的调用次数）。对
`tawcroot_build_filter` 的机械扩展：与
close 特例形式相同。

### execve 处理细节

这是设计中最棘手的部分，也是 proot 偶然复杂度
最多的地方。有两个相互交织的问题：

1. **handler 在 `execve` 后不复存在。** 按 `execve(2)`，
   已捕获信号的处置在 `execve` 时被重置为 `SIG_DFL`。
   因此虽然 seccomp 过滤器得以保留（内核状态，
   经由 `PR_SET_NO_NEW_PRIVS` 继承），SIGSYS handler 却
   不会——而 SIGSYS 的默认动作是"终止
   进程"。若我们直接 `execve` 进 ld.so，ld.so 的第一个
   带路径的系统调用（为 `libc.so.6` 发出的 `openat`）会在没有
   handler 的情况下陷入，程序在 main() 之前就死掉。
2. **内核需要为新程序找到磁盘上真实存在的动态链接器。**
   ELF 中写死的 PT_INTERP 路径
   （`/lib64/ld-linux-x86-64.so.2`）在宿主上不存在；我们
   必须重定向它。

方案 A 通过 re-exec 自身、让新的 `--exec-child` 重新安装 handler，
解决了*第一次* exec 的问题 1。但
第二次 exec——`--exec-child` 启动实际程序——
又揭开同一个伤口：任何通过 `execve(ld.so, …)` 把
控制权交给动态链接器的做法都会再次清掉 handler。

**修复方法是*不* `execve` 进 ld.so。** `--exec-child`
把二进制和 ld.so 手工加载到自己的地址空间
（mmap PT_LOAD 段，按 SysV ABI 用 argv/envp/auxv 设置栈，
跳转到 ld.so 的入口点），即 proot-loader 的方式。
我们的 handler 代码和全局变量原地不动；ld.so 运行在
地址空间中未使用的部分"之上"；ld.so 的
系统调用（不论是否带路径）继续经过我们的
handler，因为 handler 仍然安装着。这正是
proot 的 `libproot-loader.so` 所做的；我们在
`--exec-child` 的代码中进程内完成同样的流程，而不是借助单独的 ELF。

（不存在"第二次 `execve`"的捷径。我们考虑过
`execveat` 进 ld.so，再由一个
经 LD_PRELOAD 注入的小构造函数重新安装 handler，但 ld.so 必须在
任何用户构造函数运行*之前*发出系统调用——包括为 LD_PRELOAD
库本身发出的带路径 `openat`——因此
handler 被重置是致命的。手工加载是唯一能
让 handler 保持有效的机制。）

端到端流程：

```
guest:        execve("/bin/bash", argv, envp)
   │
   ▼  SIGSYS, our handler
handler:      execveat(our_binary_fd, "",
                       ["tawcroot", "--exec-child", "<NN>"],
                       envp, AT_EMPTY_PATH)
   │
   ▼  kernel runs us; seccomp filter inherited, handler is gone
--exec-child: read+close state fd, re-open rootfs fd / bind table,
              install SIGSYS
              handler, then manually load /bin/bash + its
              PT_INTERP into our address space and jump to
              ld.so's entry point — NO further execve.
   │
   ▼  in-process jump (no syscall, handler stays live)
ld.so:        opens /lib/libc.so.6 etc. via openat(AT_FDCWD, …)
              → those traps come back through our handler
              → handler translates to rootfs-relative *at calls
              → recursive case is self-healing
```

`--exec-child` 的手工加载步骤（取代被否决的
"execveat 进 ld.so" 路径）：

1. 把 `/bin/bash` 翻译为 `<rootfs>/bin/bash`。
2. 用 `openat(rootfs_fd, "bin/bash", O_RDONLY)` 打开二进制。
3. 读取 ELF 头。
4. 若是动态链接的，读取 `PT_INTERP`（通常是
   `/lib64/ld-linux-x86-64.so.2` 或 `/lib/ld-linux-aarch64.so.1`），
   翻译，并经由 rootfs fd 打开它。对静态二进制，
   跳过此步。
5. 把二进制以及（如适用）ld.so 的 PT_LOAD 段 `mmap` 进
   地址空间。使用内核选择的地址
   （PIE）加 `MAP_FIXED_NOREPLACE`，避免覆盖 tawcroot
   自己的代码/数据/堆。通过 `mprotect` 按段设置权限。
6. 构建 SysV ABI 初始栈：argv（恢复原始 argv[0]——
   不需要 `--argv0` 技巧，因为我们不是把
   ld.so 当 CLI 运行），envp（guest 的原样），auxv（针对
   刚加载的二进制合成 `AT_PHDR`、`AT_PHENT`、`AT_PHNUM`、`AT_BASE`、`AT_ENTRY`、
   `AT_EXECFN` 等）。
7. 设置栈指针并跳转到 ld.so 的入口（静态 ELF 则跳到
   二进制的 `e_entry`）。不发出系统调用。

一次 guest `execve` 的代价是*一次*宿主 exec（re-exec
自身）加上一次进程内加载。进程内加载
大致相当于内核 `execve` 内部所做的工作，
因此总耗时高于原生 exec，但避免了第二次
宿主 exec 进 ld.so。最大的好处是 handler 保持有效，
无需任何恢复信号的技巧。

**argv/envp 上限与不可回退点。** exec_state
收集上限（`TAWCROOT_EXEC_STATE_MAX_ARGS` 4096 个参数 /
`TAWCROOT_EXEC_STATE_MAX_ENV` 1024 个环境变量 / 64 KiB 打包字符串——
`include/exec_state.h`）在 `execveat` 提交*之前*检查，
因此超大的 exec 会给调用者返回真实的 `-E2BIG`。提交之后的
每个阶段（加载器的有效 argv 数组、栈
合成器）都必须至少接受收集阶段所允许的量，*再加上*
最多 `TAWC_SHEBANG_MAX_DEPTH * 2` 个由 shebang 前置的条目——
即 `include/loader_exec.h` 中的 `TAWC_EXEC_EFF_ARGV_MAX`。任何提交后阶段的上限更小
都不会产生 errno；它会以一个裸的加载器退出码、没有
stderr 的方式毁掉已经提交的进程。这个问题出现过两次（loader_stack 的 1024，loader_exec 的
eff_argv 约 264——对几百个文件做 `cat *` 悄无声息地
什么都没做）；`test_exec_via_handler.c` 中的回归测试
（`*_args_roundtrip`、`past_max_args_e2big_precommit`）以及
`test_loader_stack.c` 把这种步调一致性固定下来。

这在概念上就是 proot 加载器所做的，区别在于 proot 出于
ptrace 方面的原因单独提供一个 `libproot-loader.so`。我们
有不同的理由把加载器和运行时合并为一个二进制：
我们希望 SIGSYS handler 在新进程中任何带路径的
系统调用发生之前就已安装，这意味着在我们的 `_start` 运行之前
不能有需要加载的 `DT_NEEDED` 库。**因此 `libtawcroot.so` 是
静态链接且非 PIE 的**（见 "Known gaps" #1 和 §"Why
non-PIE"），同时包含过滤器/handler 运行时和
手工加载的程序加载器——一个二进制，一个静态 ET_EXEC
可执行文件（尽管文件名是 `.so`）。

### `/proc/self/exe`

当 guest 读取 `/proc/self/exe` 时，内核返回
实际被 `execve` 的那个文件的路径。在手工加载方式下
（§"execve 处理细节"）那就是 *libtawcroot.so 本身*——
从 guest 想要的二进制到现在，唯一的 `execveat`
是 re-exec 进我们自己；ld.so 是我们通过 mmap 加载的，
不是通过 `execve`。因此无论
guest 二进制是静态还是动态的，`/proc/self/exe` 都指向
`<nativeLibraryDir>/libtawcroot.so`，而且永远不是
guest 预期的路径。许多程序假定这个路径就是它们
以为自己正在运行的程序（Firefox 是著名的例子；glibc 的
`dlopen` 也依赖它做 `$ORIGIN` 解析）。

修复方法不是翻译——没有可以剥掉的宿主前缀——
而是*替换*。`--exec-child` 在初始化时暂存
guest 请求的 exec 路径（handler 的
`execveat` 成功时会替换进程，因此暂存不能
发生在 handler 本身——它发生在新的
`--exec-child` 进程中）。readlink/readlinkat handler 在看到以下路径时
返回暂存的 `guest_exe_path` 值：

- `/proc/self/exe`
- `/proc/<our-pid>/exe`
- `/proc/<our-tid>/exe`（单线程初始化时 tid 等于 pid，
  但多线程 guest 会看到不同的 tid；我们识别
  本进程中的任意 tid）

proot 也是这样做的；关键在于 guest
预期的值是它*最初*请求我们 execve 的路径，在我们做任何
重写之前。

第二种基于结果的替换，用于捕获那些没有可识别请求路径
却得到同一答案的 readlink——glibc realpath 对
从 `/proc/self/exe` 打开的 fd 做 `readlinkat(O_PATH-fd, "")`，
以及对这样的 fd 做 `readlink("/proc/self/fd/<n>")`：当内核的
结果等于 libtawcroot.so 自身的宿主路径时，返回暂存的 guest 路径。
当*请求*指向另一个进程的 exe 链接（`/proc/<other-pid>/exe`）时，
此检查被抑制：
每个 tawcroot guest 的内核 exe 都是同一个 libtawcroot.so，因此
单靠相等性检查会把*调用者*的 guest exe 路径交给
对别人链接的 readlink（实际观察到：`ls` 读取 bash 的链接
得到了 `/usr/bin/ls`）。跨进程的 exe 链接原样透传内核字节，
与其余跨进程 /proc 的处理相同。

对 `/proc/self/exe` 的 *open*（例如 `open("/proc/self/exe",
O_RDONLY)` 以重新读取二进制），我们经由普通的
rootfs 相对路径翻译到暂存的 guest 路径：open
会到达磁盘上真正的二进制。

`execve("/proc/self/exe", …)` 在 exec handler
（`exec_handler.c::tawcroot_exec_handler_prepare`）中得到同样的替换：
在探测/翻译之前把路径换成暂存的 guest exe 路径，因此 guest 会 re-exec 它认为
自己正在运行的二进制，而不是内核链接的目标（libtawcroot.so——
加载器随后会试图把它映射到正在运行的
实例之上，并以 `LOADER_FAIL(68)` 死掉）。这对
busybox 的 standalone-shell 模式（Debian 的配置）至关重要，ash 在该模式下
几乎每个 applet 都通过恰好这种 re-exec 运行；packages
bootstrap 方式在该 busybox 下运行 debootstrap
（notes/installation.md "Bootstrap flavors"）。由
`test_prod_rootfs.c::prod_rootfs_execve_proc_self_exe_is_self` 守护。

### `/proc/<pid>/fd/<n>` 反向翻译

对 fd magic link 做 readlink 会返回该 fd 目标在内核中的宿主路径。
当*请求*路径归类为
`/proc/<pid>/fd/<n>`（任意 pid——兄弟 tawcroot 进程共享
视图；`self`、数字以及 `task/<tid>/` 形式；`ls -l` 使用的相对 fd 的
`readlinkat(<fd of /proc/self/fd>, "<n>")` 形式
通过与 exe/cwd 相同的经由 dirfd 组合的步骤捕获）时，
结果通过 rootfs/bind 最长前缀遍历做反向翻译
——与 `getcwd` 使用的相同。视图外的结果
（`socket:[…]`、`pipe:[…]`、`/memfd:… (deleted)`、app 私有的宿主
文件）原样透传：fd 链接合法地指向视图
之外，因此与 `getcwd` 不同，我们不对其返回 ENOENT。

现场复现：Bun 编译的二进制（Claude Code 的原生构建）
通过 `open(dir)` +
`readlink(/proc/self/fd/<n>)` 规范化其 cwd，拿回了宿主 rootfs 路径，
然后在后续对该宿主路径的 stat 在视图内翻译不到任何东西时
以 "Can't access working directory" 死掉。

### `getcwd` 反向翻译

成功执行 `chdir("<rootfs-rel>/foo/bar")` 后，内核的 CWD
是 `<host-prefix>/<rootfs-rel>/foo/bar`。`getcwd` 返回的就是它。我们
拦截它，去掉宿主前缀，返回 rootfs 相对形式。
若该路径匹配某个 bind 源前缀，则映射回
bind 目标。

若宿主前缀不匹配（guest 不知怎地 `chdir` 到了
rootfs 之外），我们返回 `-ENOENT`，而不是泄露宿主
路径。proot 在这种情况下原样返回宿主路径，这是
一个小的信息泄露。

`readlink("/proc/self/cwd")` 通过同一个
函数（`tawcroot_cwd_to_guest_abs`）合成答案，包括视图外返回 -ENOENT
的立场，因此两个接口不会出现偏差。

### `/proc` magic link 约束

`/proc` 以读写方式 bind 进每个 rootfs，而匹配某个 bind 的路径
会走 `tawcroot_path_translate_with_ctx` 中的提前返回（"Bind
first … skip memo and resolver"），因此后缀在
完全没有符号链接解析的情况下到达内核。内核随后把
`/proc/<pid>/root` 解析为真实的**宿主**根——tawcroot 从不
`chroot`——于是 guest 可以落到宿主文件系统的任何位置，可读
可写。`cwd`、`fd/<n>` 和 `map_files/<n>` 形态相同。不是
漏洞利用，也不是解析器 bug：只是经由一个受支持的
bind 做普通的 `open()`。也不涉及 `..`——折叠在
翻译之前就把它消掉了，这也正是为什么折叠的约束论证
成立却不充分。

`tawcroot_path_translate`（path.c）中的两个机制堵住了这个口子，它们以
`tawcroot_proc_magic_link_classify`（proc_shadow.c）为依据，
后者与 shadow 分类器共享 pid/`task/<tid>/` 语法：

- **自身的 root 被重写，而非拒绝。** `/proc/self/root/X` 对
  chroot 过的进程意味着 `/X`，因此剩余部分针对
  当前 root 视图重新翻译——这既忠实于内核，又
  在构造上保证位于视图内。一个有界循环（每一轮严格
  缩短路径，因此嵌套会逐层展开）；耗尽上限即
  `-ELOOP`，绝不落到内核。`chroot(2)`
  模拟可以与之组合：重写的目标是当前的 root，
  无论它是什么。
- **其他一切先解析、再约束。** 对恰好是链接前缀的部分做 `readlink`，
  拼接需要继续解析的剩余部分，
  对结果做 `tawcroot_host_path_to_guest_abs`；视图之外即
  `-ENOENT`——这正是 guest 对其世界中不存在的路径
  应当相信的。不是绝对路径的目标
  （`pipe:[…]`、`socket:[…]`、`anon_inode:…`）无需约束，
  直接放行；无法读取的链接也一样，保留内核
  自己的 errno。

两者都只在操作作用于链接*所指向的东西*时才触发：
路径经由该链接继续解析，或者模式会跟随叶子。
对裸链接的 `readlink`/`lstat` 保持忠实于内核——`readlink
/proc/self/root` 仍然回答 `/`，这正是 chroot 过的进程
看到的。

**我们自己的 `fd/<n>` 和 `map_files/<n>` 有意豁免。**
guest 已经持有该 fd，因此该链接并不授予任何新东西——
这与 `translate_local` 让视图外 dirfd
落到内核解析时所划的界线相同（"there's nothing in our view to
escape into"）。这也是让 `/dev/stdin`、`/dev/fd/<n>` 和
进程替换得以工作的原因。其他进程的 fd 链接*不*
豁免：那种能力不属于我们。

残留问题，均为有意为之：

- **视图内的宿主路径仍可经由链接到达。** 规则
  是"guest 能否命名它？"，例如 rootfs 自身的宿主路径
  就命名了 guest 中相对 `/` 的文件。它是 guest
  已经拥有的 inode 的别名，不是逃逸（`..` 无法延伸它——折叠
  先执行）。只读 bind 仍然有效：RO 阶段 2 检查
  （§"只读 bind"）在同一个拼接后的宿主路径上运行。
- **跨进程的 `/proc/<pid>/*` 读取不变**——裸链接
  `readlink` 仍原样回答，依照
  docs/tawcroot/status.md §"Known gaps" 中的既定立场。
- 约束的代价是：`/proc` bind 下每个带 magic link 前缀的路径
  一次 `readlink`。这类路径在 `self/fd/<n>` 之外很少见，而后者
  是豁免的，因此热路径不受影响。

与 tawcroot 其余部分立场相同：这是带有内核形态错误的
意外约束，不是安全边界（overview.md
§"它明确不是什么"）。在进程内攻破监控器的 guest
绕过它的方式与绕过其他一切完全相同；内核
层面的强制执行属于 plans/tawcroot-landlock.md 的范畴。它所
恢复的，是让*文档中声明的*各发行版属性（notes/ando.md
§"Per-distro gating"、跨发行版数据隔离）在
监控器按所写方式工作时确实成立。

测试：`tests/hosted/test_proc_chroot.c` 中的
`hosted_proc_magic_link_classify`、
`hosted_proc_self_root_routes_to_guest_root`、
`hosted_proc_self_root_bare_link`、
`hosted_proc_other_pid_root_contained`、
`hosted_proc_cwd_link_contained`、
`hosted_proc_own_fd_links_not_contained`。

### chroot 模拟

启动器可以通过 `mount(source, target, NULL, MS_BIND, NULL)`
添加进程局部的目录、文件和 socket 映射。两个端点都必须存在；
目录不能映射到非目录上，反之亦然。
递归挂载、替换已有映射以及真实的文件系统
挂载仍不支持。该操作只更新运行时路径表；
它不提供 mount namespace，也不提供安全边界。应在一个专门的
子进程中、在启动应用线程之前使用它。fork 对表做快照，exec
用与路径解析相同的容量传递完整的表。
chroot 在 guest 坐标中重新锚定目标，包括新
root 位于某个已有 bind（例如存放在外部的 `/tmp`）之下的情况。
挂载目标和 chroot 保留翻译器所选的 guest 路由。它们
不得对宿主 inode 做反向映射：否则同一源的一个更具体的别名
可能选中不同的目标，并丢弃有效的子映射。
文件和 socket 条目使用父目录锚点加源文件名；
它们是路径映射，不是固定 inode 的内核挂载。其源路径
必须对被启动的进程及其后续 exec 保持可用。

在 Android app 沙箱内我们没有 CAP_SYS_CHROOT，因此
`chroot(2)` 无法转发给内核。返回 `-EPERM`
（最初的做法）会破坏 pacman 6.x 的 `_alpm_run_chroot`，
它无条件调用 `chroot(handle->root)`——在全新的
Manjaro 安装上，每个
安装后脚本和事务后 hook 都会失败（每次 `pacman -Syyu` 约 80 个错误）；rootfs 被留在
没有 GTK 图标缓存、GSettings schema、mime 缓存等的状态。因此
我们改为*模拟*该系统调用。

模拟实现位于 `src/chroot.c`。

非共享 VM 的 clone 调用经过 namespace handler。`CLONE_FS`
对等进程共享一小块匿名映射，发布当前规范的 root
宿主路径、原始 guest 路由、只读标志和代号。每个对等进程在
自己的 fd 表中重新打开该目录，并在分发下一个
翻译过的系统调用之前重新锚定其本地路径视图。普通 fork 和 `unshare(CLONE_FS)` 会脱离共享；exec 采用
同步后的本地快照，不保留共享。共享 VM 的栈
切换仍走内核快速路径。并发发布返回
EAGAIN，而不是在信号 handler 内等待。这是 root 视图
兼容性，不是 mount namespace，也不是文件系统隔离。

继承来的目录 fd 也可以用于 `fchdir`，随后在旧 root 之外
`chroot(".")`，与 Linux 保留 fd 的行为一致。

被 Android 拒绝的虚拟 root `security.capability` 写入，只有在
可读且格式正确的 revision 2/3 负载时才被接受。它们不安装任何 Android
capability，也不创建对 `getxattr` 可见的属性；其他 xattr
错误仍是错误。这支持的是包元数据，不是特权执行。

定义当前 root 视图的进程级状态：

- `tawcroot_rootfs_fd`——O_PATH 目录 fd。初始为
  supervisor 打开的 rootfs；每次成功的
  `chroot()` 时被（近似）原子地替换。*旧的* fd 留在保留 fd 列表中，
  使 guest 无法关闭它；它一直泄漏到进程退出（每次
  chroot 一个 fd，受典型的"启动时 chroot"惯用法限制）。
- `tawcroot_rootfs_host_path[+_len]`——当前 root 的内核规范
  宿主路径，通过对新 fd 做 `readlink("/proc/self/fd/<n>")`
  得到。`getcwd` 反向翻译、`/proc/self/maps`
  重写以及相对路径解析器都读取它。
- `tawcroot_binds[]`——每个 bind 都有 `active` 标志和一个
  以**当前 root 为基准**存储的 `dst` 字段。chroot 时，每个
  bind 的宿主坐标（`old_root_host + "/" + dst`）与
  新 root 比较：位于其内 → 去掉前缀 → 重新锚定；兄弟
  或外部 → 标记为非活动（src_fd 仍保留）；恰好
  等于新 root → 标记为非活动（rootfs_fd 的替换
  已覆盖它）。纯函数 helper 是
  `path_orchestrate.c` 中的 `tawcroot_path_binds_reanchor`。
- 常用符号链接缓存（`path.c` 中的 `g_memo`）
  针对新的 rootfs_fd 重建。旧条目缓存的是*外层* root 中的符号链接，
  若不刷新，会在 chroot 内悄无声息地路由
  到错误的目标。

`tawcroot_guest_exe_path`（用于合成 `/proc/self/exe`）
和 `tawcroot_self_host_path`（libtawcroot.so 的宿主路径）
**不受** chroot 影响。真实 Linux 行为相同：内核的
`/proc/<pid>/exe` 在 exec 时确定，并原样保留到 chroot 之后
——即使 chroot 使它在新视图内不可达。

设计可以干净处理的边界情况：

- **chroot("/")**（pacman 6.x 的情况）：所有路径翻译
  接口退化为恒等。binds_reanchor 让每个 dst
  保持不变。缓存重建重新读取相同的符号链接。
- **chroot 到不存在 / 非目录的目标**：带
  `O_DIRECTORY` 的 openat 返回内核的 errno；我们向上传递。
  不修改任何状态。
- **经由符号链接 chroot**：路径翻译器的符号链接
  解析器以 `FOLLOW` 模式运行，因此当 /foo 是
  符号链接时，chroot("/foo") 落在解析后的目标上。
- **带 `..` 链的 chroot**：由 `tawcroot_path_fold_absolute` 折叠；
  在 openat 之前钳制在*当前* root。
- **chroot 进某个 bind dst**：路径翻译器返回
  该 bind 的 `src_fd` 作为 `base_fd`；openat 落在 bind 源的
  宿主路径上；规范的 `/proc/self/fd/<new>` readlink 拿到
  它——这就是新 root。binds_reanchor 发现新
  root 的宿主路径与任何已有 bind 组合出的
  `cur_root + "/" + dst` 都没有关系（bind 源在构造上位于
  rootfs 之外），因此每个 bind——包括我们
  刚刚 chroot 进去的那个——都落入"外部"分支
  并被停用。**限制**：嵌套在被 chroot 进入的 bind dst 之下的 bind
  （一种少见的配置：bind A 在
  `usr/sub`，*同时* bind B 在 `usr/sub/inner`）也会
  被停用，而不是重新锚定为 "inner"。修复它
  需要把新 root 宿主路径除了与 rootfs 前缀比较外，还要与每个
  bind 源的宿主路径比较；不在当前负载的范围内。
- **重复 chroot**：每次调用都以*当前*状态作为
  外层 root，因此先 chroot("/usr") 再在其中 chroot("/lib")
  会再深入一层。（对非 root 而言，真实 chroot 并不严格可嵌套，
  但我们的模拟遵循内核宽松的
  姿态——与持有 CAP_SYS_CHROOT 的进程相同。）
- **内核 cwd 位于新视图之外**：`getcwd` 返回 `-ENOENT`
  （对 rootfs 视图之外的任何 cwd 都是既有做法，只是
  在 chroot 改变了边界之后才变得相关）。

`pivot_root` 仍返回 `-EPERM`。真实的 pivot_root 需要
两个独立的挂载并交换它们；我们完全不模拟挂载
（bind 不是内核挂载），因此没有什么有用的东西可以
模拟。没有目标负载会用到它。


## 只读 bind

`-b SRC:DST:ro` 把一个 bind 标记为只读：指向其中的写意图路径
翻译以 `-EROFS` 拒绝。rootfs 本身和
两字段的 bind 保持读写。使用方：内置的
系统分区 bind（`TawcrootMethod.bindSpecs` 把
`LIBHYBRIS_BIND_DIRS` 集合标记为 `:ro`——对在真实 Android 上
宿主只读的目录给出忠实于内核的 `EROFS`），APK 自带的 GPU 资源目录
（`TawcrootMethod.assetBinds` 把它们放在 `/usr/lib/{hybris,mesa-zink,gfxstream}`，
取代每个 rootfs 一份的拷贝，见 notes/installation.md §"Copy vs
bind"），暴露共享存储而又不必
信任每个 guest 程序执行删除的用户 `ExternalBind`（计划中），以及
plans/tawcroot-landlock.md 引入
`allowed_access` 的逐 bind 输入。

这里内核不会白送只读：bind 源 fd 只是
`openat(dirfd, suffix, flags)` 中作为起始 inode 的 `dirfd`——
所得 fd 的访问模式来自 openat 的标志，而真正的
`mount --bind -o ro` 需要 app 沙箱所没有的 namespace。
因此只读在翻译层模拟，其结构保证遗漏的
检查不会悄无声息地发生：

- **纯代码中的单一强制执行点。** 每个带路径的
  系统调用都汇集到 `tawcroot_path_translate_with_ctx`
  （path_orchestrate.c）；EROFS 拒绝只在那里出现一次，位于
  最终 bind 路由确定之后（缓存/符号链接遍历可能先进入
  某个 bind 再出来——只有最终的基准有意义；嵌套在
  RO dst 之下的 RW bind 按最长前缀胜出，就像 RO 挂载之下的
  RW 挂载）。handler 只*声明*意图
  （`tawcroot_path_intent`，translate 函数的一个新参数）；
  它们从不自己实现拒绝。
- **失败即关闭的默认值。** intent 枚举的零值是 WRITE，
  因此被遗忘/零初始化的意图会退化为 RO bind 上可见的 EROFS，
  而绝不是悄无声息的写穿。两种本质上
  会修改的模式（`PARENT_CREATE`、`PARENT_REMOVE`）在编排器内部
  无论声明如何都强制为写意图——
  mkdir/mknod/symlink 目标/unlink/rename/link 目标即使在
  调用点标错的情况下也被覆盖。剩余的标错面
  （open-for-write、truncate、chmod、chown、utimensat、setxattr）
  由 tests/hosted/test_ro_binds.c 中的 hosted 矩阵固定。
- **内核对数据写入做双重覆盖。** 经由 RO bind 的写模式打开
  被拒绝，因此不存在指向 RO bind 的写模式 fd；
  `write`/`mmap(PROT_WRITE, MAP_SHARED)`/`ftruncate`/
  `copy_file_range`/… 都会因 fd 的访问模式而失败，完全无需
  tawcroot 参与。
- **无状态的 fd 事实依据（阶段 2）。** 剩余部分——那些通过
  经由 RO bind 合法只读打开的 fd 修改*元数据*的系统调用——
  在调用时得出判定：readlink
  `/proc/self/fd/<n>`，然后 `tawcroot_host_path_in_ro_bind`
  （与 RO bind 源 + RO root 做最长前缀匹配）。没有
  污点表，没有需要在 dup/exec 间传递的东西，且
  经 SCM_RIGHTS 传递的 fd 也被覆盖。拦截/检查的有：`fchmod`、
  `fchown`、`utimensat(fd, NULL, …)`、`fsetxattr`、`fremovexattr`——
  均为冷路径。注意：这种基于宿主路径的事实依据不区分 dst——
  同一个宿主目录被绑定两次（一次 `:ro`，一次 RW）时，这些 fd
  检查按最长前缀、再按 bind 表顺序决定，而不是按 fd 是经由哪个 dst
  打开的。路径级检查是按路由的，
  不受影响。
- **/proc magic link（阶段 2）。** /proc bind 暴露了作用于
  *宿主* inode 的内核 magic link：`fd/<n>`/`map_files/<n>` 以
  新的访问模式重新打开（对只读打开的 fd 做 `open("/proc/self/fd/<n>", O_RDWR)`
  ——宿主挂载是 RW，因此内核不会
  像真正的 RO 挂载那样拒绝），而 `cwd`/`root` 会*穿透*解析
  到宿主目录（`cd /ro-bind; > /proc/self/cwd/x`）。每个
  最终路由是 /proc bind 的写意图翻译都会
  在 `tawcroot_path_translate`（path.c）中检查——这是每次 handler 翻译
  都会经过的非纯生产包装器
  （fs handler、socket bind、chroot、exec/加载器），因此未来的调用
  点无法跳过它；纯的 `_with_ctx` 编排器保持
  不探测内核，便于单元测试。这是唯一一个不能
  放在编排器中的检查（只有内核知道链接
  目标）：通过 `tawcroot_proc_magic_link_prefix`
  （proc_shadow.c，与 shadow 分类器共享 pid/task 语法，使
  两个匹配器不会出现偏差）匹配 `(self|thread-self|<pid>)/(task/<tid>/)?
  (fd|map_files/<n>|cwd|root)`，对恰好是链接的部分做 readlink，拼接
  需要继续解析的剩余部分，并对最终宿主路径做 RO 前缀检查
  （因此嵌套在 RO 源之下的 RW bind 仍然胜出）。

各调用点的意图：stats/readlink/getxattr/listxattr/不带 W_OK 的 access/
chdir/exec/statfs/open-for-read/chroot 目标以及
AF_UNIX `connect`/`sendto`/`sendmsg` 声明 READ；所有
修改操作声明 WRITE（openat 经由一个纯的 flags→intent 分类器
`tawcroot_openat_intent`），但带 O_CREAT 的 openat 例外，它
声明 CREATE——拒绝方式与 WRITE 完全相同，但不受下文
目标缺失时的 errno 保真规则约束（内核在叶子查找之前
就对创建返回 EROFS）。`bind()` 保持 `PARENT_CREATE`（它
创建 socket 文件），而 connect/sendto/sendmsg 改为
`FOLLOW`+READ——这本身就更忠实于内核，因为
内核在 connect 时会跟随叶子符号链接。

errno/保真说明：

- `access(W_OK)` → EROFS（只读文件系统上的内核行为；由
  意图表自然得出）。
- **叶子必须存在的写操作，目标缺失 → ENOENT/ENOTDIR。**
  对 FOLLOW/NOFOLLOW 的 WRITE 操作（chmod/chown/utimensat/truncate/
  setxattr/open-for-write/access(W_OK)），内核在 `mnt_want_write` *之前*
  查找目标，因此只有已存在的目标才会得到
  EROFS。遇到集中产生的 EROFS 时，`tawcroot_path_translate`（path.c）
  探测翻译后的路径（使用遍历自身的跟随语义），
  并透传 ENOENT / ENOTDIR；CREATE 意图和
  PARENT_* 模式跳过
  探测并保留 EROFS，与内核对创建/删除
  先检查写权限、再查找子项的顺序一致。
- `open(existing, O_RDONLY|O_CREAT)` 成功（POSIX：此时创建是
  空操作）。实现为 `handle_openat` 中的一次重试：遇到
  集中产生的 EROFS，且为不带 O_EXCL 的 O_CREAT、访问模式不含写、无
  O_TRUNC 时，去掉 O_CREAT 重新翻译；若结果是 ENOENT，则
  改写为 EROFS。这是唯一的标志特例。（缺失*父目录*时
  也报告 EROFS 而不是内核的
  ENOENT——可以接受。）
- 当路径路由为只读时，`statfs` 把 `ST_RDONLY` 或进 `f_flags`，
  使 `df` 和检测只读的安装程序看到真实情况。`fstatfs` 保持
  不拦截（已知偏差）：指向 RO bind 的 fd 报告为 RW，
  即使对同一路径的 `statfs` 显示 `ST_RDONLY`。
- 对 O_PATH fd 做 `fchmod`/`f*xattr` → 内核的 EBADF，而不是
  EROFS：在这些调用点，阶段 2 fd 检查对 O_PATH fd 让路
  （fchown 则命中伪 root 的成功吞错，与
  rootfs O_PATH fd 相同）。`utimensat(fd, NULL, …)` 是唯一一个
  合法作用于 O_PATH fd 的元数据写操作，因此它
  保留 EROFS 检查。
- `renameat2`：任一操作数落在只读上时统一返回 EROFS（
  内核对跨挂载的情形给出 EXDEV；对 `mv` 而言同样是终结性的）。
- **linkat 的源位于 RO bind 中 → EXDEV，有意为之。**
  对跨文件系统的 RO bind 这是忠实于内核的（工具会降级为拷贝）；对
  *同一文件系统*的 RO bind，宿主 linkat 会成功，并交出一个
  以 rootfs 命名、内容仍可写的硬链接——经典的
  RO bind mount 硬链接逃逸——因此我们有意偏离内核保真
  并拒绝。AT_EMPTY_PATH / `/proc/self/fd/N`
  形式的源由 linkstore 源检测已经执行的
  同一宿主路径前缀检查捕获。这一项和 statfs 是
  translate 结果中 `ro` 位仅有的使用者——用于保真，
  从不用于强制执行。
- chroot *进入* RO bind 是允许的（chroot 是读操作），并设置
  进程级全局变量 `tawcroot_root_ro`：替换之后整个
  root 视图就是该 bind 源，但路由经由
  `tawcroot_rootfs_fd`，而不是 bind 表。通过 exec_state v6
  （每个 bind 的 `bind_ro[]` + `root_ro`）跨 guest execve 传递。
  chroot 进 RW 目标会清除它。

它**不**承诺什么：它是带有
内核形态错误的意外约束，与 tawcroot 其余部分立场相同（"not a
security boundary"，overview.md）。进程内攻击和解析器
逃逸 bug 绕过它的方式与绕过一般约束完全相同——
内核层面的强制执行属于 plans/tawcroot-landlock.md 的范畴（只授予
RO 源读/执行权限，以同一个逐 bind 标志为依据）。
绕过它的 `/proc` magic link 路由已被处理：约束检查构建的
同一个拼接后宿主路径会输入
`tawcroot_host_path_in_ro_bind`（§"`/proc` magic link 约束"）。
