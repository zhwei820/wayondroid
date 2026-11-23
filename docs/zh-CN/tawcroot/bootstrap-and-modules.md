# tawcroot — 引导、入口与模块布局

## 引导与入口

`tawcroot` 由 `TawcrootMethod.startInside` 调用，它构建的 argv 大致形如：

```sh
#!/system/bin/sh
exec /data/app/~~<hash>/lib/arm64-v8a/libtawcroot.so \
     -r /data/data/me.phie.tawc/distros/<id>/rootfs \
     -b /system:/system \
     -b /vendor:/vendor \
     -b /apex:/apex \
     -b /system_ext:/system_ext \
     -b /dev:/dev \
     -b /proc:/proc \
     -b /sys:/sys \
     -b /dev/binderfs:/dev/binderfs \
     -b /data/data/me.phie.tawc:/data/data/me.phie.tawc \
     -w /root \
     -- /usr/bin/bash -l "$@"
```

`-b` 集合对应 `ChrootMounter.mountScript` 的挂载集合 + proot 方式的挂载集合
（`notes/proot.md`），包括 `/proc`、`/sys`、`/dev` 以及 libhybris 用到的 Android
系统库路径。由于我们并不真正*挂载*任何东西，这些只是 bind 表条目；宿主路径直接
被别名映射进 rootfs 视图。与 `ProotMethod` 一样，在某个 Android 版本上不存在的
bind 源（`/system_ext`，有时还有 `/dev/binderfs`）会在生成 wrapper 时被过滤掉，
而不是让 tawcroot 拒绝整个 argv。Android 的 `/linkerconfig` 被有意*排除*在集合
之外 — libhybris 需要的其中那一个文件改为在每次 spawn 时复制进 rootfs（见
[installation.md](../../installation.md) "The bionic linker config"）。

**`/dev/shm` 通过 `memfd_create` 在 handler 内模拟** — 不绑定宿主目录，没有
闪存写入开销。`/dev/shm/<name>` 下带路径的系统调用由 SIGSYS handler 拦截，并
路由到 `src/shm.c` 中一个小型的（name → memfd）表：

- `openat(O_CREAT)` 创建一个 `memfd_create(name, MFD_ALLOW_SEALING)`，在高位
  保留 fd 区间存放一个内部的**非 CLOEXEC** dup，并交给 guest 一个新的 dup。
  支持 `O_EXCL` 和 `O_TRUNC`。
- 对已存在名字的 `openat` 通过 `/proc/self/fd/<n>` 重新打开内部 fd（独立的
  打开文件描述 + 访问模式）；Android 上该操作对 memfd 会被拒绝，见下文。
- `unlinkat` 删除该名字；只要还有任何 fd 打开，段就继续存在（POSIX shm 语义，
  内核引用计数）。
- `fstatat`/`statx`/`faccessat` 为 `/dev/shm`（合成目录）和 `/dev/shm/<name>`
  （普通文件，大小取自 `fstat`，属主 uid 0）合成合理的结果。

fork+execve 模式（Mozilla 父进程 → content IPC）下的跨进程可见性通过两种机制
保持：

1. 内部 memfd 是非 CLOEXEC 的，因此能原样穿过 handler 驱动的 `execveat`
   re-exec — fd 编号相同，内核对象相同。
2. `exec_state` 把（name, fd_int）对通过 re-exec 时传递的 memfd 带过去；
   `--exec-child` 的加载器调用 `tawcroot_shm_register` 重建（name → fd）映射，
   不重新创建任何内核侧 memfd。

Mozilla 的 IPC SHM 实际上不存在争用（一个很小的自旋锁保护该表；只在
create/unlink 系统调用期间持有）。`mmap`/`ftruncate`/`mremap` 等对返回的 fd
作为真实内核操作执行 — 不再需要 handler 介入。

**Android 上的窄模式重新打开。** SELinux 拒绝对 app 自己的 memfd 执行 `open`，
而 fd 的访问模式在 `open()` 时就已固定，因此一个 memfd 只能产生一种（RDWR）
打开文件描述。Chromium（以 `O_RDWR|O_EXCL` 创建，以 `O_RDONLY` 重新打开，再
unlink）和 Firefox（只读的 `/proc/self/fd/<memfd>` 重新打开，见
[../firefox.md](../../firefox.md)）都会 `CHECK` 第二个 fd 确实是只读的。对于
这种被拒绝的 `O_RDONLY`/`O_WRONLY` 重新打开，如果该 memfd 仍是私有的 — 对
`/dev/shm` 而言：由本 pid 创建且其创建 fd 仍指向它；在所有情况下：
`/proc/self/fd` 中没有其他 fd、`/proc/self/maps` 中没有映射 — 其内容会被移到
link store 的 `tmp/` 中一个无名 `O_TMPFILE`（仅在未配置 store 时用 rootfs 目录：
`O_TMPFILE` 需要对目录有属主写权限，而 Arch 的 `/` 是 0555，这曾导致那里的每个
Firefox 标签页都崩溃），再 `dup3` 覆盖到 guest fd（以及表中的内部 fd）上，保持
fd 编号、CLOEXEC、状态标志和偏移量不变。对该文件的 `/proc/self/fd` 重新打开是
允许的，因此这次以及之后的每次打开都能得到真正的打开文件描述。否则回退到
`F_DUPFD`（可写 fd）。在两个浏览器中，触发时段都是空的，因此只有很少被写入的
只读共享区域会变成文件后端；普通 IPC 内存仍留在 memfd 上。无名意味着崩溃后没有
需要 unlink 或清扫的东西。在创建和重新打开之间 fork 出的子进程保留旧的 memfd
（可接受；两个浏览器都不这么做）。

迁移后的文件以 mode `01600 | seals` 且 nlink 0 作为标记，`fcntl` handler 根据
这些位为它们模拟 `F_ADD_SEALS`/`F_GET_SEALS`（仅凭 fd 就能识别，因此在通过
SCM_RIGHTS 拿到 fd 的沙箱子进程中也有效）。seal 只是建议性的：没有任何东西
强制执行它们。

与真实 `/dev/shm` 相比已知的低价值保真度差距（会让程序出错的那些 — O_RDONLY
访问模式、每次打开独立的文件偏移、`.`/`..` 分类、错误的 STATX_SIZE — 已通过以
guest 的访问模式经 `/proc/self/fd/<n>` 重新打开内部 memfd 修复）：

- `mode` 被忽略（shm_open 中 `(void)mode`），stat 总是合成 0600，不管创建者的
  mode 或后续的 `fchmod`。在伪 root 下没问题。
- 针对 `/dev/shm` 目录本身的 handler 行为不一致：`stat`/`statx`/`access` 把它
  伪装成存在，但对它的 `openat`/`chdir` 返回 ENOENT，并且对 `/dev/shm/<name>`
  的 `truncate`/`utimensat`/`renameat*`/`statfs` 完全没有拦截。configure 风格
  的探测会看到一个能 stat 但打不开的目录。

`-w` 设置初始 CWD（经过转换）。我们还会在 exec 前导出一小组环境变量（`HOME`、
`USER`、`TMPDIR`、`PATH`）。

## 模块布局

实际布局（扁平 — 原计划有 `syscalls/` 子目录，但文件数从未多到值得这么做；在
execve 增加 `exec.c` 之前保持为一个 `syscalls_fs.c`）。

```
tawcroot/                            # everything tawcroot-specific lives here
├── README.md           # short: "see docs/tawcroot/"
├── build               # cross-ABI NDK build (also stages into APK jniLibs)
├── build-fixtures.sh   # NDK build for guest fixtures (loader smoke)
├── test.sh             # runs the cleat orchestrator (host) or pushes
│                       #   testhost via adb (device)
├── Makefile            # incremental host build (production + testhost + cleat tests)
├── include/                            # production headers — no test scaffolding
│   ├── tawcroot.h      # entry-point contract (tawcroot_main)
│   ├── arch.h          # syscall_args struct, includes arch/<arch>.h
│   ├── arch/{aarch64,x86_64}.h  # arch_read_args / arch_write_return
│   ├── chroot.h        # chroot(2) emulation registration
│   ├── dirent_filter.h # getdents64 reserved-fd dirent compaction — pure
│   ├── dispatch.h      # syscall→handler table API
│   ├── errno_neg.h     # negated-errno constants (TAWC_EINVAL == -22)
│   ├── exec_handler.h  # execve handler entry (memfd build + execveat)
│   ├── exec_state.h    # serialized re-exec state struct + (de)serialize
│   ├── fdtab.h         # reserved high-fd table + close/dup protection
│   ├── filter.h        # seccomp filter install API
│   ├── filter_build.h  # pure cBPF program builder
│   ├── handler.h       # SIGSYS handler install + observation
│   ├── identity.h      # virtual identity struct + registration
│   ├── io.h            # libc-free print helpers + tawc_str_* builders
│   ├── loader_elf.h    # phdr parsing, image bounds, interp pointer
│   ├── loader_exec.h   # --exec-child entry + shebang resolution
│   ├── loader_jump.h   # asm-only stack-pivot + final jump to ld.so/_start
│   ├── loader_map.h    # mmap/mprotect of PT_LOADs, AT_PHDR computation
│   ├── loader_stack.h  # synthesize argv/envp/auxv on a fresh stack
│   ├── path.h          # translator, modes, bind table, open_in_view
│   ├── path_oracle.h   # readlink oracle interface used by resolver
│   ├── path_orchestrate.h # fold→bind→memo→resolve→bind ctx + memo struct
│   ├── path_resolve.h  # symlink walker — operates against an oracle
│   ├── path_scratch.h  # handler-safe PATH_MAX scratch-buffer pool
│   ├── proc_rewrite.h  # /proc/self/maps reverse-translation — pure
│   ├── proc_shadow.h   # /proc shadow fd+metadata synthesis, /proc/self classify
│   ├── raw_sys.h       # tawc_<syscall> wrappers
│   ├── shm.h           # /dev/shm emulation (memfd-backed name table)
│   ├── signal_shadow.h # guest SIGSYS sigaction/sigmask virtualization
│   ├── supervisor.h    # shared per-process bootstrap (prod + --exec-child)
│   ├── syscalls_{control,exec,fs,socket}.h # handler registration entries
│   ├── sysnr.h         # per-arch syscall numbers
│   ├── tawc_string.h   # memcpy/memset/... freestanding-vs-hosted switch
│   ├── tawc_uapi.h     # kernel ABI constants (O_*, AT_*, struct stat, ...)
│   └── usercopy.h      # process_vm_readv-based guarded copy
├── src/                                # production sources — no test scaffolding
│   ├── main.c          # production entry: CLI parse, --exec-child dispatch
│   ├── chroot.c        # chroot(2) emulation — root-view swap on guest call
│   ├── dirent_filter.c # getdents64 compaction — pure
│   ├── dispatch.c      # syscall→handler table storage
│   ├── exec_handler.c  # execve guest-side entry (build memfd state + execveat)
│   ├── exec_state.c    # exec-state (de)serialization — pure
│   ├── filter.c        # install BPF program (seccomp + stub address)
│   ├── filter_build.c  # build BPF program — pure
│   ├── handler.c       # sigsys_handler dispatch, ucontext glue
│   ├── identity.c      # stateful virtual identity handlers
│   ├── io.c            # io.h print impl (via tawc_write)
│   ├── strings.c       # pure libc-free str/mem helpers + bounded builders —
│   │                   #   also linked into the cleat test runner under hosted
│   │                   #   glibc (tawcroot/tests/unit/test_strings.c)
│   ├── path.c          # translate(), reverse-translate, bind table, memo,
│   │                   #   open_in_view
│   ├── path_fold.c     # absolute-path folder (`.`/`..`/empty/`//`) — pure
│   ├── path_orchestrate.c # fold→bind→memo→resolve→bind staging, binds_reanchor — pure
│   ├── path_resolve.c  # symlink walker — pure, oracle-driven
│   ├── path_scratch.c  # scratch-buffer pool (CAS acquire/release)
│   ├── proc_rewrite.c  # /proc/self/maps line rewriter — pure
│   ├── proc_shadow.c   # /proc shadow memfds + stat/statx/access, one classifier
│   ├── shm.c           # /dev/shm emulation
│   ├── signal_shadow.c # SIGSYS sigaction/sigmask shadow state
│   ├── supervisor.c    # shared bootstrap: rootfs fd, binds, handler, masks
│   ├── loader_elf.c    # ELF phdr parsing
│   ├── loader_map.c    # PT_LOAD mmap/mprotect, AT_PHDR computation
│   ├── loader_stack.c  # synthesize argv/envp/auxv on a fresh stack
│   ├── loader_exec.c   # --exec-child main: load guest + jump, shebang chain
│   ├── loader_io_prod.c # production loader I/O vtable (raw syscalls)
│   ├── syscalls_{fs,fd,control,exec,socket}.c # per-syscall handlers
│   ├── usercopy.c      # process_vm_readv probe + guarded guest copies
│   └── arch/{aarch64,x86_64}_{stub,loader_jump}.S  # _start, raw syscall stub,
│                                                   # sigreturn trampoline, loader jump
└── tests/                              # everything that isn't shipped in production
    ├── testhost/
    │   ├── include/
    │   │   ├── child.h     # --exec-child entry
    │   │   ├── rootfs_smoke.h # rootfs syscall smoke entry
    │   │   └── smoke.h        # foundation smoke harness
    │   └── src/
    │       ├── testhost_main.c  # argv dispatch (--exec-child / -r ROOTFS / foundation)
    │       ├── child.c          # --exec-child re-entry
    │       ├── rootfs_smoke.c   # rootfs syscall smoke (inline-asm probes)
    │       └── smoke.c          # foundation smoke (trap-contract, raw-syscall exercise)
    ├── unit/                # cleat-direct pure-function tests (no fork)
    │   └── test_strings.c   # tawc_strlen/streq/starts_with/parse_long/int_to_str
    ├── handler/             # cleat tests that fork tawcroot-testhost
    │   ├── steps.{c,h}      # parse [ok ]/[FAIL] lines from testhost stdout and
    │   │                    #   register one cleat test per check (dynamic)
    │   ├── test_foundation_smoke.c      # one dynamic test per foundation step
    │   └── test_rootfs_syscalls_smoke.c # builds fake rootfs, then rootfs smokes
    └── integration/         # cleat tests that fork production tawcroot
        └── programs/        # tiny C guests built by the runner and exec'd under tawcroot
```

该目录的布局使它可以被整体拆出成独立仓库（`tawcroot/` 内没有外部路径）。唯一
与 TAWC 应用的耦合在 `tawcroot/build.sh`，它把生产二进制暂存到
`app/src/main/jniLibs/<abi>/libtawcroot.so` 以供 APK 打包；以及
`tawcroot/test.sh --device`，它 source `scripts/lib/select-device.sh` 来选择
adb 目标 — 拆分时去掉这些即可。

构建产物（依据 `tawcroot/build.sh`）：
- **生产 tawcroot** — 每个 ABI 一个静态非 PIE 的 ET_EXEC：
  arm64-v8a / x86_64 上为 `libtawcroot.so`，宿主上为 `tawcroot`。
  出于同样的 APK-execve 原因，像 `libproot-loader.so` 一样作为 jniLib 发布。
  没有测试脚手架，没有 `--run-test`，没有冒烟驱动，没有第三方依赖。
- **`tawcroot-testhost`** — 相同源文件集 + `tawcroot/tests/testhost/src/`，
  以 `-DTAWCROOT_TESTHOST` 编译。`--abi=host` 时默认构建；交叉 ABI 仅在指定
  `--testhost` 时构建。不打包进 APK。
- **`tests`** — cleat 编排器。`--abi=host` 时默认构建（hosted glibc）；按需
  通过 `tawcroot/build.sh --abi=<abi> --tests` 针对 bionic 交叉编译为
  `aarch64`/`x86_64`。Android 变体就是 `tawcroot/test.sh --device` 推送并以
  adb shell 运行的那个；与宿主模式相同的四层测试套件、相同的过滤语法、相同的
  退出码 — 只有编排器二进制不同。