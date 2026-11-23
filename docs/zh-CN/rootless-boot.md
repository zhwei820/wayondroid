# 免 root 启动状态（Waydroid LineageOS 20，真机）

Galaxy S24（Android 17，内核 6.1，4K 页，仅 arm64，SELinux enforcing），通过
`run-as` 以应用 uid 运行，`scripts/rootless/boot-init.sh`。

## 当前进展

guest 能启动到锁屏，并可通过注入的输入使用：滑动解锁进入启动器，点击可打开
应用（设置），HOME 和 BACK 可用，输入到搜索框的文字能到达。init 运行完整的
启动序列，zygote fork 出 system_server，SurfaceFlinger 以软件方式合成（ANGLE
运行在 lavapipe 上），Waydroid 的 hwcomposer 把每一帧作为 `wl_shm` 缓冲区提交
给 `vine_wl`，后者把它写入 `w/vinefb`。尚不稳定，见下文的缺口。

## 显示路径

- `vine_wl --socket w/run/wayland-0 --vinefb w/vinefb --create WxH`
  （`app/src/main/cpp/display/wayland/`）：一个只服务一个客户端的小型
  Wayland 服务端，客户端是单窗口模式下的 Waydroid hwcomposer。全局对象有
  wl_compositor、wl_shm、xdg_wm_base、wl_output、wl_seat（触摸和键盘）；没有
  subcompositor 或 viewporter，因此客户端保持在单 surface 路径上。`w/run`
  被 bind 到 `/run/user/1000`。
- 输入：`vine_wl --input w/input` 接收 guest agent 的行（`touch`、`key`、
  `text`，格式与 `GuestProtocol.kt` 写出的一致），并作为
  wl_touch/wl_keyboard 事件发送；hwcomposer 把 evdev 事件写入
  `/dev/input/wl_{touch,keyboard}_events` FIFO，镜像中的 EventHub 把它们作为
  `wayland_touch` 和 `wayland_keyboard` 打开（Generic.kl）。Android 键码映射
  到低于 239 的 evdev 码（hwcomposer 的上限）：APP_SWITCH 没有对应码。
  `vine_wl --send w/input LINE...` 可从 shell 注入。
- `/vendor/waydroid.prop`，即 Waydroid 宿主端写入的内容：gralloc
  `default`、egl `angle`、vulkan `lvp`、`sys.use_memfd=true`、GLES 3.2、
  `ro.sf.lcd_density`，另加 `waydroid.background_start=false`（否则第一个
  窗口会被销毁，不会提交任何帧）和 `waydroid.stub_sensors_hal=1`（否则
  sensors HAL 永远不会注册，system_server 一直等待它）。

## 音频路径

- `vine_pulse --socket w/run/pulse/native`（`app/src/main/cpp/audio/pulse/`）：
  一个小型 PulseAudio 服务端（native protocol 32，无共享内存），服务于
  Waydroid 的音频 HAL，后者的 ALSA `pulse` 插件（libpulse 12）连接到
  `/run/user/1000/pulse/native`。每个播放流通过各自的 AAudio 流播放，每个
  录音流通过 AAudio 采集（AAudio 无法打开时为静音）。已在宿主上用 libpulse
  和 ALSA 插件测试；在设备上服务端能启动，但还没有任何流到达它（见缺口）。
- 镜像中的 `mediaserver`（`media.player`、`libmediaplayerservice`）只有
  32 位，因此在纯 64 位宿主上，每个 `MediaPlayer` 使用者都会等待一个永远
  不会出现的服务（铃声预览会卡住设置），编解码器列表也回退到缓慢的本地查找。
  解决这一点需要纯 64 位镜像或 32 位模拟。

## 传感器路径

- Waydroid 自带的 sensors HAL（1.0）没有任何传感器。guest 另外装上 Android 13
  模拟器的 HAL（2.1 multihal + goldfish「ranchu」sub-HAL，由
  `scripts/sensors/fetch-hal.py` 获取预编译件），SensorService 优先选它。它的
  `/dev/goldfish_pipe` 被预加载的 `libvine_sensors_shim` 换成
  `/run/user/1000/vine-sensors`，由 `vine_sensors`（`app/src/main/cpp/sensors/`）
  通过 NDK 传感器 API 读手机的传感器，按模拟器的 `qemud:sensors` 协议提供。
- HIDL 接口库（`android.hardware.sensors@2.0/2.1.so`）必须用镜像自己编译的：
  Waydroid 的 libhidlbase 在 `IBinder` 里多了一个虚函数，按 AOSP 编译的桩在
  第一次调用时就会崩溃。
- 已声明却始终注册不上的 HAL 会让 system_server 永远卡在 SensorService，所以没有
  `vine_sensors` 时，shim 在本地应答「没有传感器」。连续型传感器只在 App 处于
  前台时才有数据。

## Rootfs 准备（宿主契约）

引擎必须对解包后的镜像做的事情：应用的免 root 引擎在每次启动前于
`app/src/main/cpp/rootless/rootless_prep.cpp` 中完成，开发循环则在
`boot-init.sh` 中完成：

- 没有 AArch32 的宿主（Exynos 2400 及其他较新的 SoC）：把
  `ro.*.product.cpu.abilist*` 属性改写为仅 64 位，把 `ro.zygote` 改为
  `zygote64`；从 `init.zygote64_32.rc` 去掉 `zygote_secondary` 派生出
  `init.zygote64.rc`。否则 init 会运行无法 exec 的 32 位服务
  （`boringssl_self_test32`、`reboot_on_failure`）。
- `/dev/{binder,hwbinder,vndbinder}` 是指向 `/dev/binderfs` 的符号链接：
  libbinder 在打开驱动路径之前会先对其 `access()`。
- bind 宿主的 `/dev/ptmx` 和 `/dev/pts`：init 的 logwrapper 需要 pty
  （linkerconfig、vdc），没有它就不会生成任何 linker namespace。
- 共享存储：Android 13 只通过 FUSE 挂载 `/storage/emulated`。
  `vine-rootless.rc` 在 post-fs-data 阶段（zygote 之前）把 `/data/media`
  bind 到 `/mnt/user/0/emulated` 和 `/mnt/pass_through/0/emulated`，并把
  `/mnt/user/0` bind 到 `/storage`。`libvine_vold_shim` 被预加载进 vold
  （在 vold.rc 中加入 `setenv LD_PRELOAD`），它为 `/dev/fuse` 交出一个持有
  FUSE_INIT 的 socket（MediaProvider 的守护进程在其上启动然后空闲），并把
  vold 的 fuse 挂载和下层目录 bind 变成空操作，从而让卷达到 MOUNTED 状态。
  tawcroot 在 `MS_BIND|MS_REC`（zygote 的 `/storage` bind）时连带子挂载，
  并通过其目录最新的 bind 把 fd 的路径映射回来，因此真实路径仍位于
  `/storage/emulated` 之下。
- lmkd：`/product/etc/init/vine-rootless.rc` 用 `vine_lmkd`
  （`app/src/main/cpp/rootless/vine_lmkd.c`）替换它。真正的 lmkd 需要 PSI
  或 memcg，两者都对应用关闭；没有任何 lmkd 时，每次 oom-adj 更新都会让
  system_server 卡住约 3 秒，而不杀进程的话，guest（尤其是装了 GApps 的）会
  不断增长，直到宿主的 lmkd 杀掉整个应用。vine_lmkd 采用 system_server 给出
  的优先级，当应用的私有内存超过实例的 RAM（`--budget-mb`），或宿主的
  MemAvailable 低于 RAM 的 1/8 时（低于 1/16 时连后台应用也杀），杀掉最不
  重要的缓存应用。
- 音频 HAL：在 64_32 镜像中，Waydroid 的服务只有 32 位。`vine_hal_service`
  （`rootless/hal_service.cpp`）改为注册 64 位 passthrough 实现
  （audioserver 和 AudioService 会等待它们）。`waydroid_arm64_only` 镜像自带
  64 位服务（以及 64 位 mediaserver），予以保留。
- 没有 AArch32 的宿主还会去掉镜像中其他仅 32 位的 vendor 服务（64_32 镜像：
  omx、camera provider、cas）：删除它们的 rc 文件和 VINTF 条目，这样
  system_server 不会永远等待已声明的 IOmxStore，init 也不会不停重启它们。
  statsd 被禁用（它找不到 libstatssocket，每隔几秒重启一次）。
- 没有 keychord：`vine-rootless.rc` 覆盖 bugreport 并去掉其 `keycodes`，否则
  init 会打开输入 FIFO 并阻塞，直到 hwcomposer 写入一个事件。`/dev/input`
  每次启动时都为空。
- 启动信号：`vine-rootless.rc` 在 `sys.boot_completed=1` 时写入
  `/run/user/1000/boot_completed`；该文件出现时，引擎报告实例为运行中。
- Socket 路径：应用数据目录使 `sun_path`（108 字节）没有余量容纳
  `<instance>/rootless/run/pulse/native` 之类的路径。binderd、vine_wl 和
  vine_pulse 以 `<instance>/rootless` 为 cwd 运行并监听相对路径；完整路径放
  不下时，binder 垫片通过 bind 的目录 fd 进行连接。
- 首次启动：在 `/data/system/appops.xml` 中预置一个 SystemUI 监视的 op
  （system 包的 SYSTEM_ALERT_WINDOW）。LineageOS 20 的
  `AppOpsControllerImpl.fetchCurrentActiveOps` 遍历 `getPackagesForOps()` 时
  没有判空，在没有任何应用使用过这些 op 时 SystemUI 会反复崩溃。
- 以 guest 的 `/` 作为 cwd 启动 init：服务会继承它，而位于视图之外的 cwd 在
  `getcwd` 中读回 -ENOENT（libopenjdk 会因此在 `System_specialProperties`
  中崩溃）。
- 在 tawcroot 之外写文件要小心：rc 文件不得对 group 或 world 可写（init 会
  跳过它们；应用的 umask 为 0），并且镜像中的绝对符号链接（`/product`）在
  那里会相对于宿主解析。
- 清除上次启动的运行时状态（`/dev/__properties__`、`/dev/cgroup_info`、……）
  以及 tawcroot 的每会话 store 文件。

## 为此所需的 tawcroot / binderd 修复

- 对合成的 mountinfo fd 做 epoll_ctl 时，在 arm64 上使用了 x86_64（packed）
  的 `epoll_event` 布局，把 `data` 截断为 32 位：init 的 MountHandler 拿回一个
  被截断的指针并发生段错误。
- 被拒绝的 `security.selinux` 写入改为成功但不生效（init/apexd 中的
  restorecon 把拒绝视为致命错误）。
- `--all-caps`：伪造的 capability 从完整集合开始，而不是 nspawn 的集合；
  init 会中止 capset 被拒绝的服务（logd 的 CAP_SYSLOG）。
- 伪造的 securebits，包括 `PR_SET_KEEPCAPS`（宿主应用的是锁定的）。
- 对伪造 CAP_SYS_RESOURCE 的持有者，把超出宿主硬限制的
  setrlimit/prlimit64 钳制住（bpfloader 的 1 GiB RLIMIT_MEMLOCK）。
- `/proc/self/stat` 的 startstack 是 guest 的栈：bionic 据此确定主线程栈的
  大小，ART 的栈保护曾因此失败。
- 模拟的 mount namespace（`unshare(CLONE_NEWNS)`）：zygote 和每个应用 fork
  都会 unshare 并私有地挂载。
- binderd：一个正在退出的进程的最后一个 ref 可能唤醒 node 的属主，后者的读
  操作在 release 完成之前就释放了 node（double free，杀死了 binderd，连带
  所有服务）。
- selinuxfs：`/sys/fs/selinux/{status,enforce}` 读出为 permissive
  （installd 和 libselinux 的 status 页）。
- Netlink：被拒绝的 bind 改为成功，并且在 init 会话中 NFLOG/NETFILTER socket
  得到一个回环桩（任一失败都会使 netd 退出）。
- `/proc/<pid>/status` 显示虚拟 Uid/Gid：ActivityManager 会检查 provider 宿主
  进程的 Uid 行，缺少它时会把（托管在 system_server 中的）SettingsProvider
  当作「正在崩溃」而分离。
- SO_PEERCRED 把伪造的 init 报告为 pid 1：init 的属性服务曾把它自己设置的
  「restorecon done」当作新请求并永远循环，使 vold（以及 system_server 的
  解锁）一直等待。
- 抽象 unix socket 对所有应用是同一个 namespace：每个 guest 应用的 ADB-JDWP
  线程都连到了宿主 adbd 的 `@jdwp-control`，被拒绝后立即重连，烧掉手机的 CPU
  （adbd 加上每个 guest 应用）。现在 init 会话会给其抽象名加上
  `tawc<init pid>.` 前缀，对 guest 不可见。
- init 占满了一整个核：它的 MountHandler 倒回其 /proc/mounts fd 而不是重新
  打开，因此 tawcroot 模拟的挂载变化唤醒（一个 inotify watch，只在重新打开时
  排空）在第一次变化后就再也不会清除。现在倒回（lseek，在合成的挂载 fd 上被
  拦截）会重新渲染表并确认该变化，与内核行为一致。
- set*id 特权曾只看 euid 0：webview_zygote 不是 root，但持有（模拟的）
  CAP_SETUID/CAP_SETGID，用来把子进程移到隔离 uid，结果每个 WebView 都在
  `setresgid(99001)` 上中止。
- llvmpipe 的光栅化线程偶尔会运行一个 JIT 代码已被释放的着色器
  （`llvmpipe-N` 中的 SEGV_ACCERR，导致 SurfaceFlinger 或 SystemUI 崩溃）；
  `vine-rootless.rc` 导出 `LP_NUM_THREADS=0`，让绘制线程自行光栅化。
- binderd 在缓冲区仍指向事务目标 node 时释放了该 node（`free_buffer` 随后
  访问了已释放内存）；现在缓冲区会固定住其目标 node。
- binder 垫片的线程表位于 vfork 子进程共享的内存中：子进程关闭 binder fd
  （ART 的 Runtime.exec 子进程会关闭所有 fd）会拆掉父进程的 binder 线程，
  后者随后以 EIO 失败（SurfaceFlinger 中止；init 随之重启 zygote）。

## 已知缺口与后续

- system_server 的 watchdog 偶尔触发，此时 ActivityManager 的锁被一个 binder
  线程持有，该线程处于一个永不完成的 oneway `scheduleStopService` 中（发生在
  应用死亡之后）。自上述目标 node 修复后未再出现；如果再次出现，binderd 的
  SIGUSR1 状态转储是排查起点。
- 部分 Waydroid vendor HAL 只有 32 位（camera、cas、media.omx）：在纯 64 位
  宿主上不重新构建就没有相机。
- mdnsd 在 fd 1056+ 上的 `FD_SET` 处中止：guest 可见的 fd 处于或高于
  tawcroot 的保留下限（1000）时，会破坏 `select()` 的使用者。
- statsd 等因缺少 `libstatssocket`（APEX 库在默认 namespace 中不可达）以及
  iptables 而失败。
