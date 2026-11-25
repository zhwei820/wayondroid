# 子系统与真机对照（环境探针）

2026-11-25 第四次实测（10:05），子系统对应 `93000b2`（当前 HEAD）；
真机列沿用 2026-11-24 第二次实测（18:23），本轮未重跑。
两侧运行同一份探针 `dev.probe.env`（普通第三方应用视角，非 shell/root）：
子系统用 `scripts/probe/run.sh`，真机用同一份源码以可调试构建安装后运行。

- **子系统**：rootless guest，Waydroid LineageOS 20 镜像，身份伪装为
  Pixel 5（redfin / Android 13 / TQ3A.230901.001.C2）。
- **真机**：宿主手机 OnePlus PHK110（ColorOS 16 / Android 16 / SM8475）。
- **Pixel 5 参照**：★ 表示来自公开资料（Google 工厂镜像、公开规格），不是本机实测。

> **真机侧权限限制**：ColorOS 拒绝 shell 的 `pm grant`
> （`Neither user 2000 nor current process has GRANT_RUNTIME_PERMISSIONS`），
> 真机报告是在 **未授予运行时权限** 下跑出来的。凡依赖
> `READ_PHONE_STATE` / `ACCESS_FINE_LOCATION` / `CAMERA` / `BLUETOOTH_CONNECT`
> 的字段，真机列只代表「无权限应用」看到的值，表中标为「不可比」。
>
> 宿主真机也不是 Pixel 5。型号、SoC、屏幕、包名等机型相关值本来就应不同；
> 本对照看的是「一台真实 Android 手机提供的设备面与权限语义」和子系统的差异。

## 结论

1. **硬标记**：探针的值正则在子系统侧 `<<MARK` 为 0；`org.lineageos.*`
   对第三方应用的包查询已全部隐藏，隐式 Intent 仍可解析到它们。
2. **仍存差异（与权限无关，可直接比较）**：
   - `/proc/self/mounts` 104 行（真机 218）；`/dev/socket` 无 `zygote_secondary`
   - 相机只测了 640×480；全尺寸 4032×3024 / 3264×2448 与 RAW 未实测，第三颗超广角未暴露
   - 图形扩展集 124 vs 真机 104，缺 `GL_OES_tessellation_shader` / `GL_OES_gpu_shader5`
   - 网络接口只有 `dummy0 wlan0 ifb0 lo ifb1`（`rmnet_*` 有意不列）
   - 162 个包、无 GMS / Play（用户未要求补齐）
   - `supportedModemCount=1`（Pixel 5 有 eSIM，预期 2，未核对）
3. **探针覆盖不到、且不做修复的底层差异**：CPU ID 寄存器与指令集、真实内核 5.10 的
   系统调用集、性能与时序、GPU 驱动行为、硬件密钥认证等，见「附：硬不可修复层」。
4. **待核对值**：`Build.HOST`、传感器器件型号、`MemTotal`、`ro.build.date`、
   `phoneCount` / `supportedModemCount`、超广角相机组成。

## 本轮已修复（第四次实测，相对第三次）

| 项 | 第三次 | 本次 |
|---|---|---|
| 显示模式 | 仅 `1080x2340@90` | `1080x2340@60` + `@90` ✅（`hwc_shim.c`） |
| 传感器 | 17 个；气压计无事件；无显著运动；无 wakeup 变体 | 24 个；气压计 6 秒 86 次事件；显著运动恢复；加速度/陀螺/磁力/光感/气压/计步有 `… Wakeup` 副本 ✅ |
| 相机出图 | 两路同一张 640×480 测试图（中心 `ff008700`） | 两路为宿主相机真实帧，各 45–50 帧、内容不同 ✅（仍只测 640×480） |
| 网络接口 | `dummy0 wlan0 lo` | `dummy0 wlan0 ifb0 lo ifb1` ✅（`rmnet_*` 仍有意不列） |
| `/vendor` 内容 | etc/init 14、bin/hw 12、lib64/hw 32 | 86 / 51 / 30（Pixel 5 工厂镜像 87 / 53 / 36，vendor view 生效）✅ |
| `/proc/self/mounts` | 93 行 | 104 行（部分） |
| 应用可见包 | 138，无 Chrome | 162，含 Chrome / TrichromeLibrary ✅（无 GMS） |
| uname | `4.19.269-…`，未核对 | 与 TQ3A.230901.001.C2 的 `boot.img` 完全一致 ✅ |

第三次已验的 `ro.hardware.egl=adreno`、GL ES 3.2、传感器器件型号、Wi-Fi 两条路径一致、
LineageOS 包隐藏，本次保持。

## 1. 身份与构建

| 字段 | 子系统 | 真机 PHK110 | Pixel 5 (redfin) 参照 | 判定 |
|---|---|---|---|---|
| Build.MODEL / BRAND | Pixel 5 / google | PHK110 / OnePlus | Pixel 5 / google | ✅ |
| Build.DEVICE / PRODUCT / HARDWARE / BOARD | redfin ×4 | OP5913L1 / PHK110 / qcom / taro | redfin | ✅ |
| Build.FINGERPRINT | `google/redfin/redfin:13/TQ3A.230901.001.C2/10753682:user/release-keys` | OnePlus/PHK110/OP5913L1:16/… | 同左 | ✅ |
| Build.DISPLAY | TQ3A.230901.001.C2 | PHK110_16.0.5.1201(CN01) | 同左 | ✅ |
| Build.TAGS / TYPE / USER | release-keys / user / android-build | release-keys / user / root | 同左 | ✅ |
| Build.HOST | abfarm-release-rbe-64-2004-0087 | kvm-slave-build-s-system-13034639 | ★ 未核对 | ⚠️ |
| Build.SOC | QTI SM7250 | QTI SM8475 | QTI SM7250 | ✅ |
| Build.radio / gsm.version.baseband | `g7250-00258-230518-B-10157620` | Q_V1_P14（双卡） | ★ 同左（.C2 工厂包） | ✅ |
| ro.bootloader / Build.BOOTLOADER | `r3-0.5-9825705` | unknown | ★ 同左（.C2 工厂包） | ✅ |
| ro.product.system.device | redfin | ossi | ★ redfin | ✅ |
| ro.board.platform / ro.hardware | lito / redfin | taro / qcom | lito / redfin | ✅ |
| ro.hardware.egl / vulkan | adreno / adreno | adreno / adreno | ★ adreno / adreno | ✅ |
| ro.secure / ro.debuggable | 1 / 0 | 1 / 0 | 1 / 0 | ✅ |
| ro.boot.verifiedbootstate / flash.locked | green / 1 | green / 1 | green / 1 | ✅ |
| ro.revision / ro.boot.hardware.revision | MP1.0 / MP1.0 | 0 / 空 | ★ MP1.0 | ✅ |
| uname | `4.19.269-g8728c337137c-ab10161573 #1 SMP PREEMPT Thu May 18 11:19:41 UTC 2023` | 5.10.236-android12-9-o-g74d132f4467a | `4.19.269-g8728c337137c-ab10161573 …`（TQ3A.230901.001.C2 的 `boot.img`） | ✅ 与工厂镜像一致 |

## 2. 显示与硬件参数

| 字段 | 子系统 | 真机 PHK110 | Pixel 5 参照 | 判定 |
|---|---|---|---|---|
| display | 1080x2340 dpi=440 | 1240x2772 dpi=560 | 1080x2340 @440ppi | ✅ |
| display.modes | `1080x2340@60` + `@90` | 60 fps，备选 90/120 | ★ 60/90 两档 | ✅ |
| mem.total | 7,795,003,392（MemTotal 7,612,303 kB） | 15,957,581,824 | 8GB 机型，具体 MemTotal ⚠️ | ⚠️ |
| statfs.data | 118,648,471,552（≈110.5 GiB） | 492 GB | 128GB 机型，可用 ≈110GiB | ✅ |
| runtime.cpus / cpufreq policies | 8；policy0/6/7，1.8/2.2/2.4 GHz | 8；policy0/4/7 | ★ 同子系统（SM7250） | ✅ |
| cpuinfo part / features | 0x805×6 + 0x804×2，Hardware=LITO | 0xd46/0xd47/0xd48 | ★ 同子系统 | ✅（寄存器仍可读到真值，见盲区） |
| auxv HWCAP | 0x119fff | 0xefbfffff | 与 lito features 对应 | ✅ |
| battery | 镜像宿主（level 56 / USB / 30.5℃） | 实机 | — | ✅ |

## 3. 传感器

应用可见 24 个：LSM6DSR 加速度/陀螺（含 uncalibrated）、LIS2MDL 磁力计（含
uncalibrated）、TCS3701 接近（wakeup）/光感、BMP380 气压计、Google Step Counter /
Step Detector、显著运动，以及加速度/陀螺/磁力/光感/气压/计步的 `… Wakeup` 副本；
AOSP 融合传感器（Game/GeoMag/Rotation Vector、Gravity、Linear Acceleration、
Orientation）也在。

- **气压计有数据** ✅：注册 `TYPE_PRESSURE` 后 6 秒 86 次事件（1011.75 hPa）。
  宿主没有气压计时由 vine_sensors 合成：数小时尺度的漂移 + 几十 Pa 的噪声。
- **显著运动已恢复** ✅：sub-HAL 对 one-shot 传感器在 `setAllQemuSensors` 后按
  on-change 处理，避免无速率空转（`sensors_shim.c`）。
- **wakeup 版本** ✅：六个基传感器各有副本（事件由 `POST_SENSOR_EVENT` 复制）；
  接近与显著运动本身即 wakeup。除接近外，其余 wakeup 在第三次实测中缺失。
- 步数/步测/显著运动由宿主加速度计驱动的计步器产生；静止时步测/显著运动无事件，
  属正常行为，不是缺数据。
- 器件型号是否为 Pixel 5 实际器件 ⚠️ 待核对；传感器读数是宿主真实数据，
  不是固定值或过度平滑的模拟值。
- 无指纹、NFC：用户明确不要，属有意不同。

## 4. 相机

| 项 | 子系统 | 真机 PHK110 | Pixel 5 参照 | 判定 |
|---|---|---|---|---|
| camera.ids | 0,1 | 0,1 | ★ 后置 12.2MP + 16MP 超广角、前置 8MP | ⚠️ 超广角未暴露 |
| chars.0（后置） | pixelArray 4032×3024，level=3，焦距 4.38，朝向 90，physicalIds=[] | 未测（无权限） | ★ 12.2MP = 4032×3024 | ✅ 尺寸；level 待核对 |
| chars.1（前置） | pixelArray 3264×2448，level=1，焦距 2.51，朝向 270 | 未测 | ★ 8MP = 3264×2448 | ✅ 尺寸 |
| 实际出图 | 两路各 45–50 帧 640×480 JPEG；后摄中心 `ff0c0a0d`、前摄 `ff777777`，内容不同 | 未测（无权限） | 与特性一致的全尺寸照片 | ✅ 已是宿主相机真实帧；仍只测 640×480 |
| features | camera.full/raw/manual_sensor/autofocus/flash/front | 同左 + NFC/指纹等 | 同左 | ⚠️ 全尺寸与 RAW 输出未实测 |

宿主相机直通（`vine_camera` → `camera_shim`）现在能拿到真实帧：`camera.capture.0/1`
各 45–50 帧且两路内容不同，不再是同一张测试图。探针为省时只采 640×480；全尺寸
4032×3024 / 3264×2448 与 RAW 输出尚未验证。Pixel 5 的第三颗 16MP 超广角没有暴露
（`camera.ids=0,1`）。

## 5. 网络

| 字段 | 子系统 | 真机 PHK110 | Pixel 5 参照 | 判定 |
|---|---|---|---|---|
| net.info / iface | `1 WIFI CONNECTED` / wlan0 | 同左 | 同左 | ✅ |
| net.interfaces | `dummy0 wlan0 ifb0 lo ifb1` | `rmnet_data0 dummy0 wlan0 rmnet_data1 lo vgate0 ifb0 ifb2 ifb1` | ★ 无 SIM 时形态未核对 | ⚠️ ifb 已补；`rmnet_*` 有意不列（见下） |
| wifi.enabled / sticky | true / 3 | true / 3 | true / 3 | ✅ |
| wifi.ssid / scan | `"Xiaomi_7713"`；5 个 AP | `<unknown ssid>`；空 | 有定位权限时可见 | ⚠️ 真机无权限，不可比 |
| wifi.info | `64:09:80:60:77:13` / id=0，IP 192.168.31.105 | `02:00…` / -1 | 有定位权限时为真值 | ⚠️ 不可比 |
| net.caps（默认回调） | SSID `<unknown ssid>`、BSSID `02:00…`、Net ID -1 | 同左 | 同左 | ✅ |
| net.caps.withLocation | `"Xiaomi_7713" 64:09:80:60:77:13 id=0` | 未测 | 带位置标志时为真值 | ✅ 与 ConnectionInfo 一致 |

`rmnet_data*` 仍不列：没有 SIM 时这些 link 是 down 且没有地址，`NetworkInterface`
只列有地址的接口；真机列的 `rmnet_*` 是插卡状态。要证伪需要一台真·无 SIM 手机做参照。

## 6. 图形

| 字段 | 子系统 | 真机 PHK110 | Pixel 5 参照 | 判定 |
|---|---|---|---|---|
| gl.vendor / renderer | Qualcomm / Adreno (TM) 620 | Qualcomm / Adreno (TM) 730 | Qualcomm / Adreno 620 | ✅ |
| gl.version | `OpenGL ES 3.2 V@0490.0 (GIT@4783c89, …) (Date:11/30/20)` | `OpenGL ES 3.2 V@0615.100 …` | ★ ES 3.2 | ✅ |
| gl.extensions | 124，无 ANGLE/CHROMIUM 扩展 | 104 | 未核对 | ⚠️ |
| ES 3.2 相关扩展 | 缺 `GL_OES_tessellation_shader`、`GL_OES_gpu_shader5`，其余 8 个有 | 未测 | 未核对 | ⚠️ |
| 3.2 上下文 | 创建成功 | — | 成功 | ✅ |

字符串层面已对齐；渲染仍由 ANGLE-on-Vulkan 完成，渲染时序、shader 编译行为与
原生 Adreno 驱动不同，属字符串伪装改不到的层面。

扩展集仍有差异：子系统 124 个（ANGLE 的集合经过滤），真机 104 个；缺
`GL_OES_tessellation_shader`、`GL_OES_gpu_shader5`（只有 EXT 别名）。另有一个
非标准属性 `ro.egl.hal.impl=angle`（libEGL 改名后靠它加载 ANGLE）；探针未查该键，
应用若直接读 `SystemProperties` 可能看到 angle，待确认。

## 7. 文件系统、/proc 与权限面

| 项 | 子系统 | 真机 PHK110 | Pixel 5 参照 | 判定 |
|---|---|---|---|---|
| /proc/version、osrelease | EACCES | EACCES | 现代 Android 同样拒绝 | ✅ 非差异 |
| /proc/tty/drivers | 空 | 无 goldfish/virtio | 无 | ✅ |
| /proc/self/maps、mountinfo 标记 | 空 | 大量 oplus（宿主机型） | 应为空 | ✅ |
| /proc/self/mounts 行数 | 104（含 dm-*、f2fs /data） | 218 | 真机约 200+（Pixel 5 实际行数未核对） | ❌ 仍偏少 |
| /proc/self/cgroup | v1 memory/cpuset/cpu/blkio + `0::/uid_…/pid_…` | v1 多行 + `0::` | — | ✅ |
| /proc/self/status | Seccomp=2 / filters=1 / NoNewPrivs=1 / TracerPid=0 | filters=1 / NoNewPrivs=0（可调试） | 非调试应用 NoNewPrivs=1 | ✅ |
| 列 /dev | EACCES | EACCES | ★ 应用不可列 | ✅ |
| /dev/socket | 33 项，含 location、wpa_wlan0、qmux_radio、ims_*、usap_pool_primary、zygote；无 zygote_secondary | 55+ 项（含 OnePlus 私有、vendor_wpa_wlan0、zygote_secondary） | Pixel 5 的 `init.zygote64_32.rc` 有 zygote_secondary | ❌ 缺 zygote_secondary；根因是 guest 镜像 arm64-only |
| /vendor/etc/init / /vendor/bin/hw / /vendor/lib64/hw | 86 / 51 / 30 项 | 110 / — / — | Pixel 5 工厂镜像 87 / 53 / 36 | ✅（lib64/hw 少 6 项） |
| ls /system/bin | 414 项 | unreadable | 未核对 | ⚠️ |
| env.extra | 仅 TMPDIR | 大量 ColorOS 变量 | 少量 | ✅ |
| `exists.*` / `file.*` 标记文件 | 全 false | 全 false | 全 false | ✅ |

Pixel 5 的工厂 `system.img` 含 32 位库（`ro.system.product.cpu.abilist32=armeabi-v7a,armeabi`）
和 `init.zygote64_32.rc`，所以 `zygote_secondary` 是真差异，不是参照机型选错。补一个
同名 socket 只能骗过目录列表；真正补 32 位需要 32 位 system libs 和 32 位 tawcroot。
`vendor_wpa_wlan0` **不需要**：Pixel 5 的 supplicant rc 用 `-g@android:wpa_wlan0`，
`vendor_wpa_wlan0` 是 OnePlus 的命名。

## 8. 包

| 项 | 子系统 | 真机 PHK110 | Pixel 5 参照 | 判定 |
|---|---|---|---|---|
| 应用可见包数 | 162 | 481 | 出厂含 GMS，显著更多 | ⚠️ 仍少（用户未要求 GMS） |
| GMS / Play / Chrome / Pixel 应用 | Chrome / TrichromeLibrary（无 GMS） | 有 Google 套件 | ★ 有 | ⚠️ Chrome 已装；GMS 未补 |
| 应用可见的 LineageOS 包 | 无 | 无 | 无 | ✅ |

Chrome / TrichromeLibrary 的 APK 内有 `lib/arm64-v8a/libmonochrome_64.so`（此前记录的
“只有 v7a”已不成立）；文件来自 `files/system-apps`，按 Pixel 5 `product.img` 的目录布局安装。
没有装 GoogleCamera。

## 9. 电话与蓝牙（无 SIM，有意保留）

| 字段 | 子系统 | 真机 PHK110 | Pixel 5 无 SIM 参照 | 判定 |
|---|---|---|---|---|
| gsm.sim.state / tel.simState | ABSENT / 1 | LOADED,ABSENT / 5 | ABSENT / 1 | ✅ |
| tel.phoneType / voice / sms | 1 / true / true | 同左 | 同左 | ✅ |
| networkOperator 等 | 空 | 46001 / 中国联通 / cn | 空 | ✅ |
| serviceState | null | SecurityException（无权限） | AOSP 13 无 SIM 时为 null | ✅ 非差异（已核对 AOSP 源码） |
| phoneCount / modems | 1 / supported=1 active=1 | 2 | 实体卡 + eSIM，supported 预期 2 | ⚠️ 待核对 |
| gsm.version.ril-impl | Qualcomm RIL 1.0 | 同左 | 同左 | ✅ |
| bt | 关闭，name=Pixel 5，addr 02:00…（应用无 LOCAL_MAC 权限时的正常值） | — | 同左 | ⚠️ 开启后的行为未测 |

## 附：硬不可修复层（不做修复）

以下差异由宿主硬件、宿主内核或缺失的硬件信任根决定，用户态（props / shim / seccomp）
改不掉，**明确不做修复**。

| 层 | 具体信号 | 为什么不可修复 |
|---|---|---|
| CPU ID 寄存器 | EL0 执行 `mrs MIDR_EL1`、`ID_AA64ISAR*_EL1` 等返回宿主 SoC 真实值 | arm64 内核在 EL0 模拟这些寄存器，返回值由当前 CPU 决定，用户态无法拦截；与伪装的 `/proc/cpuinfo`、HWCAP 必然矛盾 |
| CPU 指令集能力 | 直接执行 sha3 / i8mm / sm4 等 SM7250 不支持的指令会成功 | 指令是否可用由真实 CPU 决定，seccomp 只能拦系统调用，拦不住指令执行 |
| 真实内核版本与系统调用集 | `uname` 可伪装 4.19，但 `pidfd_open`、`close_range` 等 5.x 调用真实可用 | 系统调用存在性由宿主内核决定；seccomp 可以拒绝某些调用，但无法让 5.10 内核“假装没有”这些调用 |
| 内核安全状态 | `Seccomp_filters`、SIGSYS 行为、`NoNewPrivs`、SELinux enforcing 细节 | 由宿主内核和 seccomp 机制真实决定，guest 内可读的 `/proc/self/status` 反映的是真实状态 |
| 性能与时序 | CPU 跑分远高于 Pixel 5 的 765G；tawcroot 系统调用拦截、binderd Unix socket 转发、ANGLE 渲染引入额外延迟 | 由真实执行路径和硬件性能决定，无法通过 props 或 shim 消除；计时侧信道可测出 |
| GPU 驱动真实行为 | 字符串可伪装 Adreno 620 / ES 3.2，但渲染由 ANGLE-on-Vulkan 完成；shader 编译时序、扩展真实行为、驱动私有查询与原生 Adreno 不同 | 驱动实现不同，用户态改名改不掉；GL 扩展集可过滤，但底层渲染路径和时序改不了 |
| Play Integrity / 硬件密钥认证 | 依赖真实设备 keybox、TEE/StrongBox、系统完整性状态 | rootless、无 Zygisk、无 Magisk 的环境几乎走不通；伪造 attestation 维护成本极高且合规风险不可控 |
| 物理传感器真实噪声与跨传感器相关性 | 宿主传感器数据可透传，但合成传感器（如气压计）的噪声分布、温度漂移、静止方差与真实器件不同；跨传感器相关性难以完全复现 | 没有真实 Pixel 5 传感器日志作参照就无法验证“合成得更真”；物理器件的噪声特性不可精确模拟 |
| 相机硬件能力与 ISP | 分辨率受限于手机相机流（1920×1440）；声明 level=3 / full / RAW 与实际出图可能不一致；超广角未暴露 | 真实出图能力由宿主相机硬件和 ISP 决定；可以调整声明使其与能力一致，但无法让宿主相机产出 Pixel 5 的全尺寸 / RAW 结果 |

## 附：已知的探针盲区

- 上表各层探针均未覆盖，应用可自行测出。
- **相机全尺寸**：探针为省时只采 640×480，4032×3024 / 3264×2448 与 RAW 输出没有覆盖；
  声明 level full/raw 与全尺寸实际出图是否一致仍未知。
- **mount 行数的参照**：218 行来自 Android 16 的 OnePlus 真机，不是 Pixel 5；
  Pixel 5 的实际行数未核对，只能作量级参照。
- `<<MARK` 只匹配「值」，`pkg.<包名>` 的包名在 key 上不会被标记。
- 真机侧 `pm grant` 不可用，权限相关字段只能等真机 UI 授权或换测试设备。

## 复现方法

子系统（在宿主机上执行，自动安装/卸载探针）：

```sh
scripts/probe/run.sh
```

报告写出后探针会停在实时读数页，等 guest 里点「关闭」（`files/closed`）；`run.sh`
要到它退出才卸载。界面不可见时用 `am force-stop dev.probe.env` 结束。

真机（同一份 `scripts/probe/src`，构建时加 `android:debuggable="true"`，并申请
`CAMERA`、`READ_PHONE_STATE`、`ACCESS_FINE_LOCATION`、`BLUETOOTH_CONNECT` 等权限）：

```sh
# 伪代码：aapt2/d8 打包并签名后
adb install -r probe.apk
# 注意：ColorOS 会拒绝 shell 的 pm grant；需在系统 UI 中手动授权，或换可授权的设备
for p in READ_PHONE_STATE ACCESS_FINE_LOCATION BLUETOOTH_CONNECT CAMERA; do
  adb shell pm grant dev.probe.env android.permission.$p
done
adb shell am start -W -n dev.probe.env/.Main
adb shell run-as dev.probe.env cat files/report.txt
adb uninstall dev.probe.env
```

## 参考来源

- Google Pixel 工厂镜像/OTA（TQ3A.230901.001.C2 的 bootloader、radio、kernel）：
  <https://developers.google.com/android/images>
- 2023-09 Pixel 更新与 `.C2`（Verizon）版本：
  <https://9to5google.com/2023/09/18/android-13-september-pixel-update/>
- Pixel 5 规格（90Hz、12.2MP+16MP、8MP 前摄、气压计）：
  <https://devicehelp.optus.com.au/google/pixel-5-android-11-0/specifications>
- Pixel 5 `ro.product.first_api_level=30`：
  <https://xdaforums.com/t/pixelflasher-for-google-pixel-5-support-thread.4419577/page-10>
- 工厂 `boot.img` 内核串（`4.19.269-g8728c337137c-ab10161573`，本次已核对）：
  `image-redfin-tq3a.230901.001.c2.zip` 中的 `boot.img`
- 2023-09 构建的 security_patch 仍为 2023-09-01（该版本特例）：
  <https://www.nobsbitcoin.com/grapheneos-v2023091800/>

## 附录 A：子系统探针原始输出（2026-11-25 10:05，93000b2）

```text
Build.MODEL=Pixel 5
Build.BRAND=google
Build.DEVICE=redfin
Build.PRODUCT=redfin
Build.HARDWARE=redfin
Build.BOARD=redfin
Build.FINGERPRINT=google/redfin/redfin:13/TQ3A.230901.001.C2/10753682:user/release-keys
Build.TAGS=release-keys
Build.TYPE=user
Build.HOST=abfarm-release-rbe-64-2004-0087
Build.USER=android-build
Build.DISPLAY=TQ3A.230901.001.C2
Build.SOC=QTI SM7250
Build.radio=g7250-00258-230518-B-10157620
os.version=4.19.269-g8728c337137c-ab10161573
time.build=4 ms
prop.waydroid.display_width=
prop.waydroid.stub_sensors_hal=
prop.ro.lineage.version=
prop.ro.modversion=
prop.ro.lineagelegal.url=
prop.sys.lineage_settings_system_version=
prop.ro.board.platform=lito
prop.ro.hardware=redfin
prop.ro.hardware.egl=adreno
prop.ro.hardware.vulkan=adreno
prop.ro.hardware.gralloc=
prop.ro.vendor.vine.gpu=
prop.ro.kernel.qemu=
prop.ro.boot.qemu=
prop.ro.product.system.device=redfin
prop.ro.build.user=android-build
prop.ro.bootloader=r3-0.5-9825705
prop.ro.baseband=g7250-00258-230518-B-10157620
prop.gsm.sim.state=ABSENT
prop.gsm.version.baseband=g7250-00258-230518-B-10157620
prop.gsm.version.ril-impl=Qualcomm RIL 1.0
prop.init.svc.vendor.ril-daemon=
prop.ro.secure=1
prop.ro.debuggable=0
prop.ro.boot.verifiedbootstate=green
prop.ro.boot.flash.locked=1
time.props=3 ms
tel.simState=1
tel.phoneType=1
tel.networkOperator=
tel.networkOperatorName=
tel.networkCountryIso=
tel.simOperator=
tel.voiceCapable=true
tel.smsCapable=true
tel.phoneCount=1
tel.serviceState=null
tel.activeSubs=0
time.telephony=249 ms
net.info=1 WIFI CONNECTED
net.caps=transportInfo=SSID: <unknown ssid>, BSSID: 02:00:00:00:00:00, MAC: 02:00:00:00:00:00, IP: /192.168.31.105, Security type: 2, Supplicant state: COMPLETED, Wi-Fi standard: 5, RSSI: -52, Link speed: 866Mbps, Tx Link speed: 866Mbps, Max Supported Tx Link speed: 866Mbps, Rx Link speed: 780Mbps, Max Supported Rx Link speed: 866Mbps, Frequency: 5180MHz, Net ID: -1, Metered hint: false, score: 0, isUsable: true, CarrierMerged: false, SubscriptionId: -1, IsPrimary: -1, Trusted: false, Restricted: false, Ephemeral: false, OEM paid: false, OEM private: false, OSU AP: false, FQDN: <none>, Provider friendly name: <none>, Requesting package name: <none><none>MLO Information: , AP MLD Address: <none>, AP MLO Link Id: <none>, AP MLO Affiliated links: <none> specifier=null
net.wifi=true
net.ethernet=false
net.cellular=false
net.iface=wlan0
net.interfaces=dummy0 wlan0 ifb0 lo ifb1
wifi.enabled=true
wifi.ssid="Xiaomi_7713"
wifi.scan=Xiaomi_7713/-52 Xiaomi_7713/-47 ChinaNet-D2C3/-70 CMCC-22FC/-81 TP-LINK_3F52/-74 HUAWEI-7E1D/-86
wifi.sticky=3
wifi.startScan=true broadcast=updated=true
wifi.info=64:09:80:60:77:13 rssi=-52 866Mbps 5180MHz std=5 id=0 COMPLETED ip=1763682496 mac=02:00:00:00:00:00
time.network=2400 ms
sensor.1=LSM6DSR Accelerometer | STMicro | mode=0
sensor.4=LSM6DSR Gyroscope | STMicro | mode=0
sensor.2=LIS2MDL Magnetometer | STMicro | mode=0
sensor.1.wakeup=LSM6DSR Accelerometer Wakeup | STMicro | mode=0
sensor.4.wakeup=LSM6DSR Gyroscope Wakeup | STMicro | mode=0
sensor.8.wakeup=TCS3701 Proximity | AMS | mode=1
sensor.5=TCS3701 Ambient Light | AMS | mode=1
sensor.6=BMP380 Barometer | Bosch | mode=0
sensor.2.wakeup=LIS2MDL Magnetometer Wakeup | STMicro | mode=0
sensor.14=LIS2MDL Magnetometer Uncalibrated | STMicro | mode=0
sensor.16=LSM6DSR Gyroscope Uncalibrated | STMicro | mode=0
sensor.19=Step Counter | Google | mode=1
sensor.18=Step Detector | Google | mode=3
sensor.17.wakeup=Significant Motion | Google | mode=2
sensor.5.wakeup=TCS3701 Ambient Light Wakeup | AMS | mode=1
sensor.6.wakeup=BMP380 Barometer Wakeup | Bosch | mode=0
sensor.19.wakeup=Step Counter Wakeup | Google | mode=1
sensor.35=LSM6DSR Accelerometer Uncalibrated | STMicro | mode=0
sensor.15=Game Rotation Vector Sensor | AOSP | mode=0
sensor.20=GeoMag Rotation Vector Sensor | AOSP | mode=0
sensor.9=Gravity Sensor | AOSP | mode=0
sensor.10=Linear Acceleration Sensor | AOSP | mode=0
sensor.11=Rotation Vector Sensor | AOSP | mode=0
sensor.3=Orientation Sensor | AOSP | mode=0
sensor.read.1=LSM6DSR Accelerometer | events=89 | 1.122 -0.004 9.773
sensor.read.4=LSM6DSR Gyroscope | events=89 | -0.001 0.000 0.000
sensor.read.2=LIS2MDL Magnetometer | events=88 | 36.500 2.813 -70.625
sensor.read.1.wakeup=LSM6DSR Accelerometer Wakeup | events=115 | 1.117 -0.009 9.754
sensor.read.4.wakeup=LSM6DSR Gyroscope Wakeup | events=88 | 0.000 0.000 0.000
sensor.read.8.wakeup=TCS3701 Proximity | events=1 | 5.000
sensor.read.5=TCS3701 Ambient Light | events=39 | 127.500
sensor.read.6=BMP380 Barometer | events=86 | 1011.750
sensor.read.2.wakeup=LIS2MDL Magnetometer Wakeup | events=543 | 37.313 1.813 -69.500
sensor.read.14=LIS2MDL Magnetometer Uncalibrated | events=85 | -82.375 8.813 -204.625
sensor.read.16=LSM6DSR Gyroscope Uncalibrated | events=1084 | -0.000 0.000 0.002
sensor.read.19=Step Counter | events=1 | 0.000
sensor.read.18=Step Detector | events=0 | -
sensor.read.5.wakeup=TCS3701 Ambient Light Wakeup | events=38 | 127.500
sensor.read.6.wakeup=BMP380 Barometer Wakeup | events=83 | 1011.750
sensor.read.19.wakeup=Step Counter Wakeup | events=1 | 0.000
sensor.read.35=LSM6DSR Accelerometer Uncalibrated | events=82 | 1.122 -0.028 9.768
sensor.read.15=Game Rotation Vector Sensor | events=190 | 0.039 -0.042 0.686
sensor.read.20=GeoMag Rotation Vector Sensor | events=190 | 0.039 -0.042 0.686
sensor.read.9=Gravity Sensor | events=188 | 1.118 -0.016 9.743
sensor.read.10=Linear Acceleration Sensor | events=188 | 0.004 0.011 0.030
sensor.read.11=Rotation Vector Sensor | events=188 | 0.039 -0.042 0.686
sensor.read.3=Orientation Sensor | events=186 | 273.192 0.091 6.550
time.sensors=6207 ms
gl.vendor=Qualcomm
gl.renderer=Adreno (TM) 620
gl.version=OpenGL ES 3.2 V@0490.0 (GIT@4783c89, I46ff5fc46f, 1606807783) (Date:11/30/20)
gl.extensions=124 num=124 angle=false
egl.vendor=Android
gl.es32ext=GL_EXT_geometry_shader=1 GL_OES_geometry_shader=1 GL_EXT_tessellation_shader=1 GL_OES_tessellation_shader=0 GL_EXT_gpu_shader5=1 GL_OES_gpu_shader5=0 GL_KHR_blend_equation_advanced=1 GL_OES_sample_shading=1 GL_OES_texture_buffer=1 GL_EXT_texture_cube_map_array=1
gl.context32=ok
time.gl=245 ms
proc.version=ERR FileNotFoundException: /proc/version: open failed: EACCES (Permission denied)
proc.cpuinfo.hardware=CPU part	: 0x805 ; CPU part	: 0x805 ; CPU part	: 0x805 ; CPU part	: 0x805 ; CPU part	: 0x805 ; CPU part	: 0x805 ; CPU part	: 0x804 ; CPU part	: 0x804 ; Hardware	: Qualcomm Technologies, Inc LITO ;
proc.mounts.lines=104
proc.mounts.all=/dev/block/dm-6 / ext4 ro,seclabel,relatime 0 0|tmpfs /dev tmpfs rw,seclabel,nosuid,relatime,size=3806154k,nr_inodes=951538,mode=755 0 0|devpts /dev/pts devpts rw,seclabel,relatime,mode=600,ptmxmode=000 0 0|proc /proc proc rw,relatime,gid=3009,hidepid=invisible 0 0|sysfs /sys sysfs rw,seclabel,relatime 0 0|selinuxfs /sys/fs/selinux selinuxfs rw,relatime 0 0|tmpfs /mnt rw,seclabel,nosuid,nodev,noexec,relatime,size=3806154k,nr_inodes=951538,mode=755,gid=1000 0 0|tmpfs /mnt/installer rw,seclabel,nosuid,nodev,noexec,relatime,size=3806154k,nr_inodes=951538,mode=755,gid=1000 0 0|tmpfs /mnt/androidwritable rw,seclabel,nosuid,nodev,noexec,relatime,size=3806154k,nr_inodes=951538,mode=755,gid=1000 0 0|/dev/block/dm-7 /system_ext ext4 ro,seclabel,relatime 0 0|/dev/block/dm-8 /vendor ext4 ro,seclabel,relatime 0 0|/dev/block/dm-9 /product ext4 ro,seclabel,relatime 0 0|tmpfs /apex rw,seclabel,nosuid,nodev,noexec,relatime,size=3806154k,nr_inodes=951538,mode=755 0 0|tmpfs /linker_config rw,seclabel,nosuid,nodev,noexec,relatime,size=3806154k,nr_inodes=951538,mode=755 0 0|none /dev/blkio cgroup rw,nosuid,nodev,noexec,relatime,blkio 0 0|none /sys/fs/cgroup cgroup2 rw,nosuid,nodev,noexec,relatime,memory_recursiveprot 0 0|none /dev/cpuctl cgroup rw,nosuid,nodev,noexec,relatime,cpu 0 0|none /dev/cpuset cgroup rw,nosuid,nodev,noexec,relatime,cpuset,noprefix,release_agent=/sbin/cpuset_release_agent 0 0|none /dev/memcg cgroup rw,nosuid,nodev,noexec,relatime,memory 0 0|none /dev/stune cgroup rw,nosuid,nodev,noexec,relatime,schedtune 0 0|tracefs /sys/kernel/tracing tracefs rw,seclabel,relatime,gid=3012 0 0|pstore /sys/fs/pstore pstore rw,seclabel,nosuid,nodev,noexec,relatime 0 0|bpf /sys/fs/bpf bpf rw,nosuid,nodev,noexec,relatime 0 0|none /config configfs rw,nosuid,nodev,noexec,relatime 0 0|binder /dev/binderfs binder rw,relatime,max=1048576,stats=global 0 0|/dev/block/sda10 /metadata ext4 rw,seclabel,nosuid,nodev,noatime,discard,nodelalloc,commit=1,data=journal 0 0|/dev/block/sde41 /vendor/firmware_mnt vfat ro,context=u:object_r:firmware_file:s0,relatime,gid=1000,fmask=0337,dmask=0227,codepage=437,iocharset=iso8859-1,shortname=lower,errors=remount-ro 0 0|/dev/block/sda2 /mnt/vendor/persist ext4 rw,seclabel,nosuid,nodev,noatime 0 0|fusectl /sys/fs/fuse/connections fusectl rw,relatime 0 0|/dev/block/dm-38 /data f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier 0 0|tmpfs /data_mirror rw,seclabel,nosuid,nodev,noexec,relatime,size=3806154k,nr_inodes=951538,mode=755,gid=1000 0 0|/dev/block/dm-38 /data_mirror/data_ce/null f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier 0 0|/dev/block/dm-38 /data_mirror/data_de/null f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier 0 0|/dev/block/dm-38 /data_mirror/misc_ce/null f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier 0 0|/dev/block/dm-38 /data_mirror/misc_de/null f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier 0 0|/dev/block/dm-38 /data_mirror/cur_profiles f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier 0 0|/dev/block/dm-38 /data_mirror/ref_profiles f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier 0 0|/dev/block/dm-38 /data_mirror/data_ce/null/0 f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier 0 0|/dev/block/dm-38 /data/user/0 f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier 0 0|/dev/block/dm-10 /apex/com.android.apex.cts.shim@1 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-10 /apex/com.android.apex.cts.shim ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-11 /apex/com.android.i18n@1 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-11 /apex/com.android.i18n ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-12 /apex/com.android.runtime@1 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-12 /apex/com.android.runtime ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-13 /apex/com.android.vndk.v33@1 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-13 /apex/com.android.vndk.v33 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-14 /apex/com.android.adbd@331314022 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-14 /apex/com.android.adbd ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-15 /apex/com.android.adservices@331418080 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-15 /apex/com.android.adservices ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-16 /apex/com.android.appsearch@331311000 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-16 /apex/com.android.appsearch ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-17 /apex/com.android.art@331413030 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-17 /apex/com.android.art ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-18 /apex/com.android.btservices@331716000 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-18 /apex/com.android.btservices ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-19 /apex/com.android.cellbroadcast@331411000 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-19 /apex/com.android.cellbroadcast ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-20 /apex/com.android.conscrypt@331411000 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-20 /apex/com.android.conscrypt ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-21 /apex/com.android.extservices@331412000 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-21 /apex/com.android.extservices ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-22 /apex/com.android.ipsec@331310000 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-22 /apex/com.android.ipsec ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-23 /apex/com.android.media@331410000 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-23 /apex/com.android.media ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-24 /apex/com.android.media.swcodec@331410000 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-24 /apex/com.android.media.swcodec ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-25 /apex/com.android.mediaprovider@331412040 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-25 /apex/com.android.mediaprovider ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-26 /apex/com.android.neuralnetworks@331310000 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-26 /apex/com.android.neuralnetworks ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-27 /apex/com.android.ondevicepersonalization@330442000 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-27 /apex/com.android.ondevicepersonalization ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-28 /apex/com.android.os.statsd@331311000 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-28 /apex/com.android.os.statsd ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-29 /apex/com.android.permission@331411000 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-29 /apex/com.android.permission ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-30 /apex/com.android.resolv@331314010 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-30 /apex/com.android.resolv ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-31 /apex/com.android.scheduling@331111000 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-31 /apex/com.android.scheduling ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-32 /apex/com.android.sdkext@331410000 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-32 /apex/com.android.sdkext ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-33 /apex/com.android.tethering@331412110 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-33 /apex/com.android.tethering ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-34 /apex/com.android.tzdata@331314010 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-34 /apex/com.android.tzdata ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-35 /apex/com.android.uwb@331410010 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-35 /apex/com.android.uwb ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-36 /apex/com.android.wifi@331414000 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-36 /apex/com.android.wifi ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-37 /apex/com.google.mainline.primary.libs@331434000 ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/block/dm-37 /apex/com.google.mainline.primary.libs ext4 ro,dirsync,seclabel,nodev,noatime 0 0|/dev/fuse /mnt/user/0/emulated fuse rw,lazytime,nosuid,nodev,noexec,noatime,user_id=0,group_id=0,allow_other 0 0|tmpfs /storage rw,seclabel,nosuid,nodev,noexec,relatime,size=3806154k,nr_inodes=951538,mode=755,gid=1000 0 0|/dev/fuse /storage/emulated fuse rw,lazytime,nosuid,nodev,noexec,noatime,user_id=0,group_id=0,allow_other 0 0|/dev/block/dm-38 /storage/emulated/0/Android/data f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier 0 0|/dev/block/dm-38 /storage/emulated/0/Android/obb f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier 0 0|/dev/block/dm-38 /data/misc/profiles/cur f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier 0 0|/dev/block/dm-38 /data/misc/profiles/ref f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier 0 0|/dev/block/dm-38 /data/misc/profiles/cur/0/dev.probe.env f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier 0 0|/dev/block/dm-38 /data/misc/profiles/ref/dev.probe.env f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier 0 0|
proc.mounts.0=/dev/block/dm-38 /data/misc/profiles/cur/0/dev.probe.env f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier 0 0
proc.mounts.1=/dev/block/dm-38 /data/misc/profiles/ref/dev.probe.env f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier 0 0
file./dev/qemu_pipe=false
file./dev/goldfish_pipe=false
file./dev/socket/qemud=false
file./dev/binderfs=true
file./system/lib64/libc_malloc_debug_qemu.so=false
file./vendor/bin/hw/libgoldfish-rild=false
file./sys/qemu_trace=false
file./vendor/lib64/hw/gralloc.vine.so=false
file./vendor/lib64/hw/vulkan.gfxstream.so=false
file./system/bin/wificond=true
file./vendor/etc/init/vine-radio.rc=false
file./proc/tty/drivers=true
proc.tty.drivers=
ls./=37 entries
ls./system=15 entries
ls./system/bin=414 entries
ls./system/bin/hw=2 entries
ls./system/etc/init=61 entries
ls./system/lib64=667 entries
ls./system/addon.d=0 entries
ls./vendor=21 entries
ls./vendor/bin/hw=51 entries
ls./vendor/etc/init=86 entries
ls./vendor/lib64=736 entries
ls./vendor/lib64/hw=30 entries
ls./product/etc=13 entries
ls./product/etc/init=1 entries
ls./product/etc/permissions=7 entries
ls./system_ext/etc/init=1 entries
ls./system_ext/etc/permissions=10 entries
ls./vendor/etc/permissions=57 entries
ls./vendor/etc/vintf/manifest=33 entries
dir./run=unreadable
dir./run/user/1000=unreadable
dir./dev/socket=adbd audioserver chre dnsproxyd fwmarkd ims_datad ims_qmid lmkd location logd logdr logdw mdns mdnsd msm_irqbalance netmgr port-bridge pps prng_seeder property_service qmux_radio qwes_ipc ssgtzd statsdw tombstoned_crash tombstoned_intercept tombstoned_java_trace traced_consumer traced_producer usap_pool_primary wifihal wpa_wlan0 zygote
dir./dev=unreadable
env.extra=TMPDIR=/data/user/0/dev.probe.env/cache
exists./vendor/waydroid.prop=false
exists./system/etc/init/init.waydroid.rc=false
exists./product/etc/vine=false
exists./system/bin/vine_lmkd=false
exists./vendor/lib64/libcuttlefish_fs.so=false
exists./system/addon.d/50-lineage.sh=false
proc.self.maps.marks=
time.files=748 ms
display=1080x2340 dpi=440 xdpi=440.0
display.mode={id=1, width=1080, height=2340, fps=60.000004, alternativeRefreshRates=[90.0]}
mem.total=7795003392
proc.meminfo=MemTotal:        7612303 kB ; MemFree:           78413 kB ; MemAvailable:    2141981 kB ;
statfs.data=118648471552 free=78663790592
runtime.cpus=8
cpufreq=0:1804800/300000 rel=0 1 2 3 4 5 midr=0x00000000517f805e; 1:1804800/300000 rel=0 1 2 3 4 5 midr=0x00000000517f805e; 2:1804800/300000 rel=0 1 2 3 4 5 midr=0x00000000517f805e; 3:1804800/300000 rel=0 1 2 3 4 5 midr=0x00000000517f805e; 4:1804800/300000 rel=0 1 2 3 4 5 midr=0x00000000517f805e; 5:1804800/300000 rel=0 1 2 3 4 5 midr=0x00000000517f805e; 6:2208000/652800 rel=6 midr=0x0000000051ff804e; 7:2400000/806400 rel=7 midr=0x0000000051ff804e;
cpufreq.policies=policy0 policy6 policy7
sys.cpu.possible=0-7 online=0-7
proc.cpuinfo.full=processor	: 0|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp|CPU implementer	: 0x51|CPU architecture: 8|CPU variant	: 0x7|CPU part	: 0x805|CPU revision	: 14||processor	: 1|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp|CPU implementer	: 0x51|CPU architecture: 8|CPU variant	: 0x7|CPU part	: 0x805|CPU revision	: 14||processor	: 2|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp|CPU implementer	: 0x51|CPU architecture: 8|CPU variant	: 0x7|CPU part	: 0x805|CPU revision	: 14||processor	: 3|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp|CPU implementer	: 0x51|CPU architecture: 8|CPU variant	: 0x7|CPU part	: 0x805|CPU revision	: 14||processor	: 4|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp|CPU implementer	: 0x51|CPU architecture: 8|CPU variant	: 0x7|CPU part	: 0x805|CPU revision	: 14||processor	: 5|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp|CPU implementer	: 0x51|CPU architecture: 8|CPU variant	: 0x7|CPU part	: 0x805|CPU revision	: 14||processor	: 6|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp|CPU implementer	: 0x51|CPU architecture: 8|CPU variant	: 0xf|CPU part	: 0x804|CPU revision	: 14||processor	: 7|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp|CPU implementer	: 0x51|CPU architecture: 8|CPU variant	: 0xf|CPU part	: 0x804|CPU revision	: 14||Hardware	: Qualcomm Technologies, Inc LITO|
uname.osrelease=ERR FileNotFoundException: /proc/sys/kernel/osrelease: open failed: EACCES (Permission denied)
uname=4.19.269-g8728c337137c-ab10161573 | #1 SMP PREEMPT Thu May 18 11:19:41 UTC 2023
proc.self.cgroup=4:memory:/|3:cpuset:/top-app|2:cpu:/top-app|1:blkio:/|0::/uid_10139/pid_8783|
proc.self.mountinfo.marks=
proc.self.status=TracerPid:	0 ; NoNewPrivs:	1 ; Seccomp:	2 ; Seccomp_filters:	1 ; Cpus_allowed_list:	0-7 ;
camera.ids=0,1
camera.capture.0=frames=50 jpeg 7476 640x480 px(320,240)=ff0c0a0d detail=0.13
camera.capture.1=frames=45 jpeg 11801 640x480 px(320,240)=ff777777 detail=0.88
camera.chars.0=facing=1 level=3 pixelArray=4032x3024 physical=5.6448x4.2336 focal=[4.38] orientation=90 caps=[0, 1, 2, 5, 6, 4, 7, 3, 18, 19] maxJpeg=4032x3024 physicalIds=[]
camera.chars.1=facing=0 level=1 pixelArray=3264x2448 physical=3.6556802x2.74176 focal=[2.51] orientation=270 caps=[0, 1, 2, 5, 6, 4, 7, 3, 18, 19] maxJpeg=3264x2448 physicalIds=[]
display.modes=1080x2340@60.000004 1080x2340@90.0
tel.modems=supported=1 active=1
net.caps.withLocation="Xiaomi_7713" 64:09:80:60:77:13 id=0
sensor.pressure=BMP380 Barometer | Bosch = 1011.75 hPa
features.hw=android.hardware.bluetooth android.hardware.camera.autofocus android.hardware.camera.flash android.hardware.camera.front android.hardware.bluetooth_le android.hardware.camera android.hardware.camera.capability.manual_post_processing android.hardware.camera.any android.hardware.camera.capability.raw android.hardware.camera.capability.manual_sensor android.hardware.camera.level.full android.sofware.nfc.beam
bt=false name=Pixel 5 addr=02:00:00:00:00:00
battery=level=17 plugged=2 status=2 volt=3623 temp=318 tech=Li-ion
auxv=3=78fbe38040 9=78fbe3a000 16=119fff
Build.BOOTLOADER=r3-0.5-9825705
prop.revision=MP1.0 hw=MP1.0 bootmode=unknown
gl.egl.prop=adreno
time.hw=9912 ms
pkg.count=162
```

## 附录 B：真机探针原始输出（2026-11-24 18:23）

真机侧运行时权限未授予（`pm grant` 被 ColorOS 拒绝）；`pkg.*` 行有约 180 条
OnePlus/ColorOS 私有包（全部命中 MARK，与本项目无关），此处省略，只保留
`pkg.count`；完整输出可按上文方法复现。

```text
Build.MODEL=PHK110
Build.BRAND=OnePlus
Build.DEVICE=OP5913L1
Build.PRODUCT=PHK110
Build.HARDWARE=qcom
Build.BOARD=taro
Build.FINGERPRINT=OnePlus/PHK110/OP5913L1:16/BP2A.250605.015/T.20e88f8_46f605_46f603:user/release-keys
Build.TAGS=release-keys
Build.TYPE=user
Build.HOST=kvm-slave-build-s-system-13034639
Build.USER=root
Build.DISPLAY=PHK110_16.0.5.1201(CN01)
Build.SOC=QTI SM8475
Build.radio=Q_V1_P14,Q_V1_P14
os.version=5.10.236-android12-9-o-g74d132f4467a
prop.waydroid.display_width=
prop.waydroid.stub_sensors_hal=
prop.ro.lineage.version=
prop.ro.modversion=
prop.ro.lineagelegal.url=
prop.sys.lineage_settings_system_version=
prop.ro.board.platform=taro
prop.ro.hardware=qcom
prop.ro.hardware.egl=adreno
prop.ro.hardware.vulkan=adreno
prop.ro.hardware.gralloc=
prop.ro.vendor.vine.gpu=
prop.ro.kernel.qemu=
prop.ro.boot.qemu=
prop.ro.product.system.device=ossi
prop.ro.build.user=root
prop.ro.bootloader=unknown
prop.ro.baseband=msm
prop.gsm.sim.state=LOADED,ABSENT
prop.gsm.version.baseband=Q_V1_P14,Q_V1_P14
prop.gsm.version.ril-impl=Qualcomm RIL 1.0
prop.init.svc.vendor.ril-daemon=
prop.ro.secure=1
prop.ro.debuggable=0
prop.ro.boot.verifiedbootstate=green
prop.ro.boot.flash.locked=1
tel.simState=5
tel.phoneType=1
tel.networkOperator=46001
tel.networkOperatorName=中国联通
tel.networkCountryIso=cn
tel.simOperator=46001
tel.voiceCapable=true
tel.smsCapable=true
tel.phoneCount=2
tel.serviceState=java.lang.SecurityException: getServiceStateForSubscriber
tel.activeSubs=java.lang.SecurityException: Need READ_PHONE_STATE, READ_PRIVILEGED_PHONE_STATE, or carrier privilege
net.info=1 WIFI CONNECTED
net.caps=transportInfo=SSID: <unknown ssid>, BSSID: 02:00:00:00:00:00, MAC: 02:00:00:00:00:00, IP: /192.168.5.139, Security type: 4, Supplicant state: COMPLETED, Wi-Fi standard: 11ax, RSSI: -50, Link speed: 1729Mbps, Tx Link speed: 1729Mbps, Max Supported Tx Link speed: 2401Mbps, Rx Link speed: 1253Mbps, Max Supported Rx Link speed: 2401Mbps, Frequency: 5220MHz, Net ID: -1, Metered hint: false, score: 60, isUsable: true, CarrierMerged: false, SubscriptionId: -1, IsPrimary: -1, Trusted: true, Restricted: false, Ephemeral: false, OEM paid: false, OEM private: false, OSU AP: false, FQDN: <none>, Provider friendly name: <none>, Requesting package name: <none><none>MLO Information: , Is TID-To-Link negotiation supported by the AP: false, AP MLD Address: <none>, AP MLO Link Id: <none>, AP MLO Affiliated links: <none>, Vendor Data: <none> specifier=null
net.wifi=true
net.ethernet=false
net.cellular=false
net.iface=wlan0
net.interfaces=rmnet_data0 dummy0 wlan0 rmnet_data1 lo vgate0 ifb0 ifb2 ifb1 
wifi.enabled=true
wifi.ssid=<unknown ssid>
wifi.scan=
wifi.sticky=3
wifi.startScan=false broadcast=none
wifi.info=02:00:00:00:00:00 rssi=-51 1729Mbps 5220MHz std=6 id=-1 COMPLETED ip=-1962563392 mac=02:00:00:00:00:00
sensor.32=Dynamic Sensor Manager | Google
sensor.1=icm4x6xx Accelerometer Non-wakeup | TDK-Invensense
sensor.2=ak0991x Magnetometer Non-wakeup | akm
sensor.3=Rotation Vector  Non-wakeup | QTI
sensor.4=icm4x6xx Gyroscope Non-wakeup | TDK-Invensense
sensor.5=tcs3720 Oplus Ambient Light Sensor Non-wakeup | oplus    <<MARK
sensor.8=tcs3720 Proximity Sensor Wakeup | oplus    <<MARK
sensor.9=gravity  Non-wakeup | QTI
sensor.10=linear_acceleration | QTI
sensor.11=Rotation Vector  Non-wakeup | QTI
sensor.14=ak0991x Magnetometer-Uncalibrated Non-wakeup | akm
sensor.15=Game Rotation Vector  Non-wakeup | QTI
sensor.16=icm4x6xx Gyroscope-Uncalibrated Non-wakeup | TDK-Invensense
sensor.17=sns_smd  Wakeup | QTI
sensor.18=pedometer  Non-wakeup | oplus    <<MARK
sensor.19=pedometer  Non-wakeup | oplus    <<MARK
sensor.20=sns_geomag_rv  Non-wakeup | QTI
sensor.22=pick_up_motion  Wakeup | oplus    <<MARK
sensor.27=device_orient  Non-wakeup | oplus    <<MARK
sensor.29=stationary_detect | QTI
sensor.30=motion_detect | QTI
sensor.35=icm4x6xx Accelerometer-Uncalibrated Non-wakeup | TDK-Invensense
sensor.33171001=tcs3720 Oplus RGB Sensor Non-wakeup | oplus    <<MARK
sensor.33171024=sensor_logger  Non-wakeup | oplus    <<MARK
sensor.33171028=tcs3720 Oplus Wise_RGB Sensor Non-wakeup | oplus    <<MARK
sensor.33171032=lux_aod  Wakeup | oplus    <<MARK
sensor.33171034=pedometer_minute  Wakeup | oplus    <<MARK
sensor.33171036=elevator_detect  Wakeup | oplus    <<MARK
sensor.33171037=oplus_activity_recognition  Wakeup | oplus    <<MARK
sensor.33171052=stk37600 Oplus RGB Sensor Rear Non-wakeup | oplus    <<MARK
sensor.33171054=stk37600 Oplus Flicker Sensor Rear Non-wakeup | oplus    <<MARK
sensor.33171055=stk37600 Oplus Ambient Light Sensor Rear Non-wakeup | oplus    <<MARK
sensor.33171060=camera_protect  Wakeup | oplus    <<MARK
sensor.33171066=gesture_prox gesture prox Sensor Wakeup | oplus    <<MARK
sensor.33171067=phone_prox phone prox Sensor Wakeup | oplus    <<MARK
sensor.33171075=pocket pocket Sensor Wakeup | oplus    <<MARK
sensor.33171089=flight_scene | oplus    <<MARK
gl.vendor=Qualcomm
gl.renderer=Adreno (TM) 730
gl.version=OpenGL ES 3.2 V@0615.100 (GIT@b82916be04, I507ba793b7, 1754480107) (Date:08/06/25)
gl.extensions=104 num=104 angle=false
egl.vendor=Android
proc.version=ERR FileNotFoundException: /proc/version: open failed: EACCES (Permission denied)
proc.cpuinfo.hardware=CPU part	: 0xd46 ; CPU part	: 0xd46 ; CPU part	: 0xd46 ; CPU part	: 0xd46 ; CPU part	: 0xd47 ; CPU part	: 0xd47 ; CPU part	: 0xd47 ; CPU part	: 0xd48 ; 
proc.mounts.lines=218
proc.mounts.0=overlay /mnt/vendor/my_stock/etc/Omoji overlay ro,seclabel,noatime,lowerdir=/mnt/opex/com.oplus.Omoji@16000011/etc/Omoji:/my_stock/etc/Omoji,override_creds=off 0 0    <<MARK
proc.mounts.1=/dev/block/by-name/oplusreserve2 /mnt/vendor/oplusreserve ext4 rw,seclabel,nosuid,nodev,noatime 0 0    <<MARK
proc.mounts.2=/dev/block/by-name/oplusreserve2 /mnt/oplus/op2 ext4 rw,seclabel,nosuid,nodev,noatime 0 0    <<MARK
proc.mounts.3=/dev/block/loop5 /mnt/opex/com.oplus.AutoLayout@14001010 ext4 ro,dirsync,seclabel,nodev,noatime 0 0    <<MARK
proc.mounts.4=/dev/block/loop13 /mnt/opex/com.oplus.CustCore@16000008 ext4 ro,dirsync,seclabel,nodev,noatime 0 0    <<MARK
proc.mounts.5=/dev/block/loop21 /mnt/opex/com.oplus.NetworkAssistSys@16001066 ext4 ro,dirsync,seclabel,nodev,noatime 0 0    <<MARK
proc.mounts.6=/dev/block/loop30 /mnt/opex/com.oplus.Omoji@16000011 ext4 ro,dirsync,seclabel,nodev,noatime 0 0    <<MARK
proc.mounts.7=/dev/block/loop43 /mnt/opex/com.oplus.fancyIconLoader@1000000 ext4 ro,dirsync,seclabel,nodev,noatime 0 0    <<MARK
proc.mounts.8=/dev/block/loop45 /mnt/opex/com.oplus.moduleservices@15000014 ext4 ro,dirsync,seclabel,nodev,noatime 0 0    <<MARK
proc.mounts.9=/dev/block/loop46 /mnt/opex/com.oplus.odmf@16003008 ext4 ro,dirsync,seclabel,nodev,noatime 0 0    <<MARK
proc.mounts.10=overlay /system_ext/framework/oplusex/com.oplus.AutoLayout overlay ro,seclabel,noatime,lowerdir=/mnt/opex/com.oplus.AutoLayout@14001010/javalib:/system_ext/framework/oplusex/com.oplus.AutoLayout,override_creds=off 0 0    <<MARK
proc.mounts.11=overlay /system_ext/framework/oplusex/com.oplus.CustCore overlay ro,seclabel,noatime,lowerdir=/mnt/opex/com.oplus.CustCore@16000008/javalib:/system_ext/framework/oplusex/com.oplus.CustCore,override_creds=off 0 0    <<MARK
proc.mounts.12=overlay /system_ext/framework/oplusex/com.oplus.NetworkAssistSys overlay ro,seclabel,noatime,lowerdir=/mnt/opex/com.oplus.NetworkAssistSys@16001066/javalib:/system_ext/framework/oplusex/com.oplus.NetworkAssistSys,override_creds=off 0 0    <<MARK
proc.mounts.13=overlay /system_ext/lib64/oplusex/com.oplus.NetworkAssistSys overlay ro,seclabel,noatime,lowerdir=/mnt/opex/com.oplus.NetworkAssistSys@16001066/lib64:/system_ext/lib64/oplusex/com.oplus.NetworkAssistSys,override_creds=off 0 0    <<MARK
proc.mounts.14=overlay /system_ext/framework/oplusex/com.oplus.fancyIconLoader overlay ro,seclabel,noatime,lowerdir=/mnt/opex/com.oplus.fancyIconLoader@1000000/javalib:/system_ext/framework/oplusex/com.oplus.fancyIconLoader,override_creds=off 0 0    <<MARK
proc.mounts.15=overlay /system_ext/framework/oplusex/com.oplus.moduleservices overlay ro,seclabel,noatime,lowerdir=/mnt/opex/com.oplus.moduleservices@15000014/javalib:/system_ext/framework/oplusex/com.oplus.moduleservices,override_creds=off 0 0    <<MARK
proc.mounts.16=overlay /system_ext/lib64/oplusex/com.oplus.moduleservices overlay ro,seclabel,noatime,lowerdir=/mnt/opex/com.oplus.moduleservices@15000014/lib64:/system_ext/lib64/oplusex/com.oplus.moduleservices,override_creds=off 0 0    <<MARK
proc.mounts.17=overlay /system_ext/framework/oplusex/com.oplus.odmf overlay ro,seclabel,noatime,lowerdir=/mnt/opex/com.oplus.odmf@16003008/javalib:/system_ext/framework/oplusex/com.oplus.odmf,override_creds=off 0 0    <<MARK
proc.mounts.18=overlay /system_ext/lib64/oplusex/com.oplus.odmf overlay ro,seclabel,noatime,lowerdir=/mnt/opex/com.oplus.odmf@16003008/lib64:/system_ext/lib64/oplusex/com.oplus.odmf,override_creds=off 0 0    <<MARK
proc.mounts.19=/dev/block/dm-28 /my_product erofs ro,seclabel,relatime,user_xattr,acl,cache_strategy=readaround 0 0
proc.mounts.20=/dev/block/dm-29 /my_engineering erofs ro,seclabel,relatime,user_xattr,acl,cache_strategy=readaround 0 0
proc.mounts.21=/dev/block/dm-30 /my_company erofs ro,seclabel,relatime,user_xattr,acl,cache_strategy=readaround 0 0
proc.mounts.22=/dev/block/dm-31 /my_carrier erofs ro,seclabel,relatime,user_xattr,acl,cache_strategy=readaround 0 0
proc.mounts.23=/dev/block/dm-32 /my_region erofs ro,seclabel,relatime,user_xattr,acl,cache_strategy=readaround 0 0
proc.mounts.24=/dev/block/dm-33 /my_heytap erofs ro,seclabel,relatime,user_xattr,acl,cache_strategy=readaround 0 0
proc.mounts.25=/dev/block/dm-34 /my_stock erofs ro,seclabel,relatime,user_xattr,acl,cache_strategy=readaround 0 0
proc.mounts.26=overlay /my_stock/etc/Omoji overlay ro,seclabel,noatime,lowerdir=/mnt/opex/com.oplus.Omoji@16000011/etc/Omoji:/my_stock/etc/Omoji,override_creds=off 0 0    <<MARK
proc.mounts.27=/dev/block/dm-35 /my_preload erofs ro,seclabel,relatime,user_xattr,acl,cache_strategy=readaround 0 0
proc.mounts.28=/dev/block/dm-36 /my_bigball erofs ro,seclabel,relatime,user_xattr,acl,cache_strategy=readaround 0 0
proc.mounts.29=/dev/block/dm-37 /my_manifest erofs ro,seclabel,relatime,user_xattr,acl,cache_strategy=readaround 0 0
proc.mounts.30=tmpfs /my_reserve tmpfs rw,seclabel,nosuid,nodev,noexec,relatime,size=7773868k,nr_inodes=1943467,mode=755,gid=1000 0 0
proc.mounts.31=/dev/block/by-name/oplusreserve2 /data/persist_log/oplusreserve/media/log/shutdown ext4 rw,seclabel,nosuid,nodev,noatime 0 0    <<MARK
proc.mounts.32=/dev/block/by-name/oplusreserve2 /data/persist_log/oplusreserve/media/log/hang_oplus ext4 rw,seclabel,nosuid,nodev,noatime 0 0    <<MARK
proc.mounts.33=/dev/block/by-name/oplusreserve2 /data/persist_log/cache/factory ext4 rw,seclabel,nosuid,nodev,noatime 0 0    <<MARK
proc.mounts.34=/dev/block/by-name/oplusreserve2 /data/persist_log/oplus_fsck_fulldiskscanuserdata ext4 rw,seclabel,nosuid,nodev,noatime 0 0    <<MARK
proc.mounts.35=/dev/block/by-name/oplusreserve2 /data/persist_log/criticallog ext4 rw,seclabel,nosuid,nodev,noatime 0 0    <<MARK
proc.mounts.36=/dev/block/by-name/oplusreserve2 /data/persist_log/oplusreserve/media/log/minidumpbackup ext4 rw,seclabel,nosuid,nodev,noatime 0 0    <<MARK
proc.mounts.37=/dev/block/by-name/oplusreserve2 /data/persist_log/storage/op2storagelog ext4 rw,seclabel,nosuid,nodev,noatime 0 0    <<MARK
proc.mounts.38=/dev/block/dm-73 /data/user_de/0/dev.probe.env f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier,compress_algorithm=lz4,compress_log_size=2,compress_mode=user,memory=normal 0 0
proc.mounts.39=/dev/block/dm-73 /data/data/dev.probe.env f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier,compress_algorithm=lz4,compress_log_size=2,compress_mode=user,memory=normal 0 0
proc.mounts.40=/dev/block/dm-73 /data/misc/profiles/cur/0/dev.probe.env f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier,compress_algorithm=lz4,compress_log_size=2,compress_mode=user,memory=normal 0 0
proc.mounts.41=/dev/block/dm-73 /data/misc/profiles/ref/dev.probe.env f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,background_gc=on,discard,no_heap,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,extent_cache,mode=adaptive,active_logs=6,reserve_root=32768,resuid=0,resgid=1065,inlinecrypt,alloc_mode=default,checkpoint_merge,fsync_mode=nobarrier,compress_algorithm=lz4,compress_log_size=2,compress_mode=user,memory=normal 0 0
file./dev/qemu_pipe=false
file./dev/goldfish_pipe=false
file./dev/socket/qemud=false
file./dev/binderfs=true
file./system/lib64/libc_malloc_debug_qemu.so=false
file./vendor/bin/hw/libgoldfish-rild=false
file./sys/qemu_trace=false
file./vendor/lib64/hw/gralloc.vine.so=false
file./vendor/lib64/hw/vulkan.gfxstream.so=false
file./system/bin/wificond=true
file./vendor/etc/init/vine-radio.rc=false
file./proc/tty/drivers=true
proc.tty.drivers=
ls./=unreadable
ls./system=15 entries 
ls./system/bin=unreadable
ls./system/bin/hw=unreadable
ls./system/etc/init=64 entries oplus_update_preopt.rc     <<MARK
ls./system/lib64=937 entries lib_oplus_security.so liboplus_resctrl_psi.so liboplusbluetooth_jni.so liboplusmmdebug.so liboplusremotedisplay.so liboplusremotedisplayclient.so liboplusutiljni.so liboplusvideoboostclient.so liboplusvideoboostservice.so libvirtio_media.dylib.so libvirtio_sys.dylib.so oplus_netd_aidl_interface-cpp.so oplus_performance_shared.so vendor.oplus.hardware.binaural_record-V1-ndk.so vendor.oplus.hardware.cameraextension-V1-ndk.so vendor.oplus.hardware.cameraextension@1.0.so vendor.oplus.hardware.cammidasservice-V1-ndk.so vendor.oplus.hardware.cammidasservice@1.0.so vendor.oplus.hardware.commondcs-V1-ndk.so vendor.oplus.hardware.cwb-V2-ndk.so vendor.oplus.hardware.cwb@1.0.so vendor.oplus.hardware.displaycolorfeature-V1-ndk.so vendor.oplus.hardware.displaypanelfeature-V1-ndk.so vendor.oplus.hardware.handlefactory-V1-ndk.so vendor.oplus.hardware.nfcExtns-V1-ndk.so vendor.oplus.hardware.olc2-V3-ndk.so vendor.oplus.hardware.opluslights-V1-ndk.so vendor.oplus.hardware.performance-V1-ndk.so vendor.oplus.hardware.performance-V2-ndk.so vendor.oplus.hardware.pogo_keyboard-V1-ndk.so vendor.oplus.hardware.sendextcamcmd-V2-ndk.so vendor.oplus.hardware.virtual_device.audio-V1-ndk.so vendor.oplus.hardware.virtual_device.camera.manager-V1-ndk.so     <<MARK
ls./system/addon.d=unreadable
ls./vendor=16 entries oplus     <<MARK
ls./vendor/bin/hw=unreadable
ls./vendor/etc/init=110 entries 
ls./vendor/lib64=1050 entries libreference-ril.so vendor.oplus.hardware.appradioaidl-V1-ndk_platform.so vendor.oplus.hardware.cameraextension-V1-ndk_platform.so vendor.oplus.hardware.cammidasservice-V1-ndk_platform.so vendor.oplus.hardware.cammidasservice@1.0.so vendor.oplus.hardware.commondcs-V1-ndk_platform.so vendor.oplus.hardware.communicationcenter-V1-ndk_platform.so vendor.oplus.hardware.communicationcenter-V2-ndk_platform.so vendor.oplus.hardware.cwb@1.0.so vendor.oplus.hardware.ims-V1-ndk_platform.so vendor.oplus.hardware.olc2-V1-ndk_platform.so vendor.oplus.hardware.osense.client-V1-ndk_platform.so vendor.oplus.hardware.performance-V1-ndk_platform.so vendor.oplus.hardware.radio-V2-ndk_platform.so vendor.oplus.hardware.sendextcamcmd-V2-ndk_platform.so vendor.oplus.hardware.wifi.hostapd@1.0.so vendor.oplus.hardware.wifi.supplicant-V4-ndk_platform.so     <<MARK
ls./vendor/lib64/hw=53 entries 
ls./product/etc=18 entries 
ls./product/etc/init=6 entries init.oplus.nandswap.rc     <<MARK
ls./product/etc/permissions=18 entries privapp-permissions-oplus.xml com.oplus.camera.unit.sdk_product.xml oplus_camera_default_grant_permissions_list.xml     <<MARK
ls./system_ext/etc/init=75 entries init.oplus.addcgroup.rc init.oplus.custom_logo.rc init.oplus.debug.rc init.oplus.rootdir.rc init.oplus.storage.occupied.rc init.oplus.upgradebeta.rc init.oplus.video.rc init.sys.oplus.connectivity.rc init.sys.oplus.mem.rc init.sys.oplus.mm.rc oplusHmbirdBpfManager.rc oplus_gaia.rc oplus_kswapd_profile.rc oplus_log_olc.rc oplus_performance_bench.rc oplus_system_debug.rc oplus_trace.rc oplus_winscope_init.rc opluscvt.rc     <<MARK
ls./system_ext/etc/permissions=78 entries com.oplus.features_config.xml com.oplus.features_secrecy.xml oplus-smart-sidebar.xml oplus-subsys-tool.xml oplus-subsystem-service.xml oplus.system.features_multimedia.xml oplus_feature_multiapp.xml privapp-permissions-oplus-common-ext.xml privapp-permissions-oplus-common.xml privapp-permissions-oplus-domestic.xml vendor-oplus-hardware-biometrics-fido.xml vendor-oplus-hardware-biometrics-fido2.xml vendor-oplus-hardware-biometrics-fingerprintpay.xml vendor-oplus-hardware-charger.xml vendor-oplus-hardware-cryptoeng.xml vendor-oplus-hardware-engineer.xml vendor-oplus-hardware-keymint.xml vendor-oplus-hardware-misc.xml vendor-oplus-hardware-oplusSensor.xml vendor-oplus-hardware-osml.xml vendor-oplus-hardware-pogo_keyboard.xml vendor-oplus-hardware-riskdetect.xml vendor-oplus-hardware-rpmh.xml vendor-oplus-hardware-stability-oproject.xml vendor-oplus-hardware-touch.xml vendor-oplus-hardware-transmessage-aidl.xml vendor-oplus-hardware-transmessage.xml vendor-oplus-hardware-weaver.xml     <<MARK
ls./vendor/etc/permissions=45 entries 
ls./vendor/etc/vintf/manifest=37 entries oplus_wifi_hostapd_device_without_wigig.xml vendor.oplus.hardware.wifi.supplicant.xml     <<MARK
dir./run=unreadable
dir./run/user/1000=unreadable
dir./dev/socket=aconfigd_system adbd adpl_cmd_uds_file audioserver dnsproxyd fwmarkd hidl2touchdaemon_client hidl2touchdaemon_server ims_datad ipacm_log_file lmkd location logd logdr logdw mdns mlid netmgr notify-topology oplus_dnsproxyd opluscvtmanager ot-daemon port-bridge postmand pps prng_seeder property_service qcc_app qcc_trd qcrild qmux_radio qti_dpm_uds_file qwes_ipc shsusrd ssgqmig ssgtzd ssgtzd_opener statsdw tcmd thermal-recv-client thermal-recv-passive-client thermal-send-client thermal-send-rule tombstoned_crash tombstoned_intercept tombstoned_java_trace traced_consumer traced_producer usap_pool_ocomp usap_pool_primary usap_pool_secondary vendor_wpa_wlan0 wifihal wigig zygote zygote_ocomp zygote_secondary    <<MARK
dir./dev=unreadable
env.extra=CUST_LEVEL_LIST=/system:/system_ext:/vendor:/odm:/my_preload:/my_stock:/my_region/global:/my_region/global/P:/my_product:/my_region:/my_region/P:/my_product/cust/CN:/my_carrier:/my_bigball:/my_company:/my_engineering MEMTAG_OPTIONS=off MY_BIGBALL_ROOT=/my_bigball MY_CARRIER_ROOT=/my_carrier MY_COMPANY_ROOT=/my_company MY_COUNTRY_ROOT=/my_region MY_ENGINEERING_ROOT=/my_engineering MY_ENGINEER_ROOT=/my_engineering MY_HEYTAP_ROOT=/my_heytap MY_MANIFEST_ROOT=/my_manifest MY_ODM_ROOT=/odm MY_OPERATOR_ROOT=/my_carrier MY_PRELOAD_ROOT=/my_preload MY_PRODUCT_ROOT=/my_product MY_REGION_ROOT=/my_region MY_RESERVE_ROOT=/my_reserve MY_SPECIAL_PRELOAD_ROOT=/special_preload MY_STOCK_ROOT=/my_stock TMPDIR=/data/user/0/dev.probe.env/cache 
exists./vendor/waydroid.prop=false
exists./system/etc/init/init.waydroid.rc=false
exists./product/etc/vine=false
exists./system/bin/vine_lmkd=false
exists./vendor/lib64/libcuttlefish_fs.so=false
exists./system/addon.d/50-lineage.sh=false
proc.self.maps.marks=aa09c000-aa3bc000 rw-p 00000000 00:00 0                                  [anon:dalvik-/system/framework/boot-oplus-framework.art] ; ab6c0000-ab720000 r--p 00000000 fe:16 16656981                           /system/framework/arm64/boot-oplus-framework.oat ; ab720000-ab856000 r-xp 00060000 fe:16 16656981                           /system/framework/arm64/boot-oplus-framework.oat ; ab85c000-ab87e000 rw-p 00000000 fe:16 11365343                           /system/framework/boot-oplus-framework.vdex ; 7839cc6000-7839cc8000 r--p 00000000 fe:17 15642343                       /system_ext/lib64/liboplushwui_jni.so ; 7839cc8000-7839cc9000 r-xp 00002000 fe:17 15642343                       /system_ext/lib64/liboplushwui_jni.so ; 7839cc9000-7839cca000 r--p 00003000 fe:17 15642343                       /system_ext/lib64/liboplushwui_jni.so ; 7839cca000-7839ccb000 rw-p 00004000 fe:17 15642343                       /system_ext/lib64/liboplushwui_jni.so ; 783f1a1000-783f1a2000 r--p 00000000 fe:17 15642328                       /system_ext/lib64/liboplusextzawgyi.so ; 783f1a2000-783f1a3000 r-xp 00001000 fe:17 15642328                       /system_ext/lib64/liboplusextzawgyi.so ; 783f1a3000-783f1a4000 r--p 00002000 fe:17 15642328                       /system_ext/lib64/liboplusextzawgyi.so ; 783f415000-783f417000 r--p 00000000 fe:17 15642089                       /system_ext/lib64/liboplus_framework_jni.so ; 783f417000-783f419000 r-xp 00002000 fe:17 15642089                       /system_ext/lib64/liboplus_framework_jni.so ; 783f419000-783f41a000 r--p 00004000 fe:17 15642089                       /system_ext/lib64/liboplus_framework_jni.so ; 783f41a000-783f41b000 rw-p 00005000 fe:17 15642089                       /system_ext/lib64/liboplus_framework_jni.so ; 783f4a5000-783f65b000 r--s 00725000 fe:17 11947874                       /system_ext/framework/oplus-framework-res.apk ; 783fe82000-783fe85000 r--p 00000000 fe:17 15642334                       /system_ext/lib64/liboplusgui_jni.so ;     <<MARK
display=1240x2772 dpi=560 xdpi=449.942
display.mode={id=2, width=1240, height=2772, fps=60.000004, vsync=60.000004, synthetic=false, alternativeRefreshRates=[90.0, 120.00001, 120.00001], supportedHdrTypes=[2, 3, 4]}
mem.total=15957581824
proc.meminfo=MemTotal:       15583576 kB ; MemFree:          342732 kB ; MemAvailable:    3787508 kB ; 
statfs.data=492494188544 free=336333680640
runtime.cpus=8
cpufreq=0:2016000/300000 rel=0 1 2 3 midr=0x00000000410fd463; 1:2016000/300000 rel=0 1 2 3 midr=0x00000000410fd463; 2:2016000/300000 rel=0 1 2 3 midr=0x00000000410fd463; 3:2016000/300000 rel=0 1 2 3 midr=0x00000000410fd463; 4:2745600/633600 rel=4 5 6 midr=0x00000000412fd470; 5:2745600/633600 rel=4 5 6 midr=0x00000000412fd470; 6:2745600/633600 rel=4 5 6 midr=0x00000000412fd470; 7:3187200/787200 rel=7 midr=0x00000000412fd480; 
cpufreq.policies=policy0 policy4 policy7
sys.cpu.possible=0-7 online=0-7
proc.cpuinfo.full=processor	: 0|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc dcpop sha3 sm3 sm4 asimddp sha512 asimdfhm dit uscat ilrcpc flagm sb paca pacg dcpodp flagm2 frint i8mm bti|CPU implementer	: 0x41|CPU architecture: 8|CPU variant	: 0x0|CPU part	: 0xd46|CPU revision	: 3||processor	: 1|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc dcpop sha3 sm3 sm4 asimddp sha512 asimdfhm dit uscat ilrcpc flagm sb paca pacg dcpodp flagm2 frint i8mm bti|CPU implementer	: 0x41|CPU architecture: 8|CPU variant	: 0x0|CPU part	: 0xd46|CPU revision	: 3||processor	: 2|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc dcpop sha3 sm3 sm4 asimddp sha512 asimdfhm dit uscat ilrcpc flagm sb paca pacg dcpodp flagm2 frint i8mm bti|CPU implementer	: 0x41|CPU architecture: 8|CPU variant	: 0x0|CPU part	: 0xd46|CPU revision	: 3||processor	: 3|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc dcpop sha3 sm3 sm4 asimddp sha512 asimdfhm dit uscat ilrcpc flagm sb paca pacg dcpodp flagm2 frint i8mm bti|CPU implementer	: 0x41|CPU architecture: 8|CPU variant	: 0x0|CPU part	: 0xd46|CPU revision	: 3||processor	: 4|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc dcpop sha3 sm3 sm4 asimddp sha512 asimdfhm dit uscat ilrcpc flagm sb paca pacg dcpodp flagm2 frint i8mm bti|CPU implementer	: 0x41|CPU architecture: 8|CPU variant	: 0x2|CPU part	: 0xd47|CPU revision	: 0||processor	: 5|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc dcpop sha3 sm3 sm4 asimddp sha512 asimdfhm dit uscat ilrcpc flagm sb paca pacg dcpodp flagm2 frint i8mm bti|CPU implementer	: 0x41|CPU architecture: 8|CPU variant	: 0x2|CPU part	: 0xd47|CPU revision	: 0||processor	: 6|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc dcpop sha3 sm3 sm4 asimddp sha512 asimdfhm dit uscat ilrcpc flagm sb paca pacg dcpodp flagm2 frint i8mm bti|CPU implementer	: 0x41|CPU architecture: 8|CPU variant	: 0x2|CPU part	: 0xd47|CPU revision	: 0||processor	: 7|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc dcpop sha3 sm3 sm4 asimddp sha512 asimdfhm dit uscat ilrcpc flagm sb paca pacg dcpodp flagm2 frint i8mm bti|CPU implementer	: 0x41|CPU architecture: 8|CPU variant	: 0x2|CPU part	: 0xd48|CPU revision	: 0||
uname.osrelease=ERR FileNotFoundException: /proc/sys/kernel/osrelease: open failed: EACCES (Permission denied)
uname=5.10.236-android12-9-o-g74d132f4467a | #1 SMP PREEMPT Fri Apr 10 03:27:19 UTC 2026
proc.self.cgroup=5:freezer:/|4:memory:/apps/dev.probe.env|3:cpuset:/top-app|2:cpu:/top-app|1:blkio:/|0::/apps/uid_10448/pid_17129|
proc.self.mountinfo.marks=24234 24233 0:110 / /mnt/vendor/my_stock/etc/Omoji ro,noatime master:74 - overlay overlay ro,seclabel,lowerdir=/mnt/opex/com.oplus.Omoji@16000011/etc/Omoji:/my_stock/etc/Omoji,override_creds=off ; 24238 24226 8:90 / /mnt/vendor/oplusreserve rw,nosuid,nodev,noatime master:19 - ext4 /dev/block/by-name/oplusreserve2 rw,seclabel ; 24250 24226 8:90 / /mnt/oplus/op2 rw,nosuid,nodev,noatime master:19 - ext4 /dev/block/by-name/oplusreserve2 rw,seclabel ; 24251 24226 7:40 / /mnt/opex/com.oplus.AutoLayout@14001010 ro,nodev,noatime master:66 - ext4 /dev/block/loop5 ro,dirsync,seclabel ; 24252 24226 7:104 / /mnt/opex/com.oplus.CustCore@16000008 ro,nodev,noatime master:68 - ext4 /dev/block/loop13 ro,dirsync,seclabel ; 24253 24226 7:168 / /mnt/opex/com.oplus.NetworkAssistSys@16001066 ro,nodev,noatime master:70 - ext4 /dev/block/loop21 ro,dirsync,seclabel ; 24254 24226 7:240 / /mnt/opex/com.oplus.Omoji@16000011 ro,nodev,noatime master:73 - ext4 /dev/block/loop30 ro,dirsync,seclabel ; 24255 24226 7:344 / /mnt/opex/com.oplus.fancyIconLoader@1000000 ro,nodev,noatime master:75 - ext4 /dev/block/loop43 ro,dirsync,seclabel ; 24256 24226 7:360 / /mnt/opex/com.oplus.moduleservices@15000014 ro,nodev,noatime master:77 - ext4 /dev/block/loop45 ro,dirsync,seclabel ; 24257 24226 7:368 / /mnt/opex/com.oplus.odmf@16003008 ro,nodev,noatime master:80 - ext4 /dev/block/loop46 ro,dirsync,seclabel ; 24264 24263 0:94 / /system_ext/framework/oplusex/com.oplus.AutoLayout ro,noatime master:67 - overlay overlay ro,seclabel,lowerdir=/mnt/opex/com.oplus.AutoLayout@14001010/javalib:/system_ext/framework/oplusex/com.oplus.AutoLayout,override_creds=off ; 24265 24263 0:98 / /system_ext/framework/oplusex/com.oplus.CustCore ro,noatime master:69 - overlay overlay ro,seclabel,lowerdir=/mnt/opex/com.oplus.CustCore@16000008/javalib:/system_ext/framework/oplusex/com.oplus.CustCore,override_creds=off ; 24266 24263 0:102 / /system_ext/framework/oplusex/com.oplus.NetworkAssistSys ro,noatime master:71 - overlay overlay ro,seclabel,lowerdir=/mnt/opex/com.oplus.NetworkAssistSys@16001066/javalib:/system_ext/framework/oplusex/com.oplus.NetworkAssistSys,override_creds=off ;     <<MARK
proc.self.status=TracerPid:	0 ; NoNewPrivs:	0 ; Seccomp:	2 ; Seccomp_filters:	1 ; Cpus_allowed_list:	0-7 ; 
camera.ids=0,1
camera.capture.0=ERR java.lang.SecurityException: validateClientPermissionsLocked:1834: Caller "dev.probe.env" (PID 17129, UID 10448) cannot open camera "0" without camera permission
camera.capture.1=ERR java.lang.SecurityException: validateClientPermissionsLocked:1834: Caller "dev.probe.env" (PID 17129, UID 10448) cannot open camera "1" without camera permission
features.hw=oppo.back.touch.fingerprint.sensor android.hardware.fingerprint android.hardware.bluetooth android.hardware.camera.autofocus android.hardware.camera.concurrent oppo.hardware.fingerprint.optical.support android.hardware.camera.flash android.hardware.camera.front android.hardware.nfc android.hardware.bluetooth_le oplus.hardware.fingerprint.unlock_without_icon.support android.hardware.camera android.hardware.camera.capability.manual_post_processing android.hardware.camera.any android.hardware.camera.capability.raw android.hardware.camera.capability.manual_sensor android.hardware.camera.level.full android.hardware.nfc.hcef android.hardware.nfc.uicc android.hardware.nfc.any android.hardware.nfc.ese android.hardware.nfc.hce     <<MARK
bt=ERR java.lang.SecurityException: Need android.permission.BLUETOOTH_CONNECT permission for android.content.AttributionSource@95b991c9: getName
battery=level=64 plugged=2 status=2 volt=3836 temp=268 tech=8
auxv=16=efbfffff 3=588de40040 9=588de42000 26=22181 
Build.BOOTLOADER=unknown
prop.revision=0 hw= bootmode=reboot
gl.egl.prop=adreno
pkg.count=481
```
