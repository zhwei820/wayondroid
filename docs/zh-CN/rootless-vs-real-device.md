# 子系统与真机对照（环境探针）

2026-11-24 第二次实测，对应提交 `580583c`（APK 18:18 构建，实例 18:19 重新准备）。
两侧运行同一份探针 `dev.probe.env`（普通第三方应用视角，非 shell/root）：
子系统用 `scripts/probe/run.sh`，真机用同一份源码以可调试构建安装后运行。

- **子系统**：rootless guest，Waydroid LineageOS 20 镜像，身份伪装为
  Pixel 5（redfin / Android 13 / TQ3A.230901.001.C2），相机 HAL 已包含在内。
- **真机**：宿主手机 OnePlus PHK110（ColorOS 16 / Android 16 / SM8475）。
- **Pixel 5 参照**：★ 表示来自公开资料（Google 工厂镜像、公开规格），不是本机实测。

> **真机侧权限限制（本次新增结论）**：ColorOS 拒绝 shell 的 `pm grant`
> （`Neither user 2000 nor current process has GRANT_RUNTIME_PERMISSIONS`），
> 真机报告是在 **未授予运行时权限** 下跑出来的。凡依赖
> `READ_PHONE_STATE` / `ACCESS_FINE_LOCATION` / `CAMERA` / `BLUETOOTH_CONNECT`
> 的字段，真机列只代表「无权限应用」看到的值，不能与已授权的子系统直接比较；
> 表中标为「不可比」。
>
> 宿主真机也不是 Pixel 5。型号、SoC、屏幕、包名等机型相关值本来就应不同；
> 本对照看的是「一台真实 Android 手机提供的设备面与权限语义」和子系统的差异。

## 结论

1. **硬标记**：探针的值正则（waydroid/lineage/goldfish/qemu/cuttlefish/
   virtio/vine/tawc/emulator/oplus …）在子系统侧 `<<MARK` 为 0；但**包名仍命中
   2 条**：`org.lineageos.jelly`、`org.lineageos.aperture`，第三方应用可见。
   探针只对「值」打 `<<MARK`，包名在 key 上，所以它们存在但不带标记。shell 里
   另有 4 个 `org.lineageos.*`，应用侧被 hide 规则挡住。
2. **本轮已修复并经探针验证**：bootloader/radio 与 fingerprint 自洽；
   system 分区产品属性恢复为 redfin/Pixel 5；`first_api_level`/`vendor.api_level`
   回到 30；屏幕 90Hz；`ConnectivityManager` 恢复默认网络；`/dev` 对应用
   不可读；`/dev/socket` 不再暴露 gralloc/kgsl-0；TransportInfo 的 BSSID 已脱敏。
3. **仍存差异（与权限无关，可直接比较）**：
   - GL 仍是 ES 3.1、`ro.hardware.egl=angle`（Pixel 5 是 ES 3.2 / adreno）
   - 网络接口只有 `wlan0 lo`（真机 9 个）
   - 应用可见 mount 66 行（真机 218）
   - 传感器缺气压计、计步、显著运动、wakeup、gyro uncalibrated
   - 相机是 2 个 640×480 测试图（Pixel 5 12.2MP+16MP/8MP）
   - `/dev/socket` 缺 `vendor_wpa_wlan0`、`location`、`zygote_secondary`
   - 140 个包、无 GMS；Jelly/Aperture 对应用可见
4. **Wi-Fi 脱敏仍有内部矛盾**：`getConnectionInfo()` 仍返回真实 BSSID
   `64:09:80:72:4f:5f` 和 `id=0`，而 `NetworkCapabilities` 的 TransportInfo
   已给 `02:00:00:00:00:00` / `id=-1`。同一权限下两条路径不一致；真机因
   权限未授予无法定论，需在真机上授予定位权限后复测。
5. **待核对值**：`uname` 内核串、`tel.serviceState=null`、`phoneCount=1`、
   `MemTotal`、`Build.HOST`、`ro.build.date`。

## 本轮已修复（探针验证）

| 项 | 上一版 | 本版 |
|---|---|---|
| bootloader | `r3-0.5-9430389` | `r3-0.5-9825705` ✅ |
| radio / baseband | `g7250-00220-221017-B-9183951` | `g7250-00258-230518-B-10157620` ✅ |
| ro.product.system.device/name/model | generic/mainline/mainline | redfin/redfin/Pixel 5 ✅ |
| ro.product.first_api_level / vendor.api_level | 28 / 28 | 30 / 30 ✅ |
| display.mode | 60 fps | 90 fps ✅ |
| ConnectivityManager | 无默认网络（net.info=null） | `WIFI CONNECTED`、iface=wlan0 ✅ |
| /dev 对应用 | 可枚举（24 个节点） | EACCES ✅ |
| /dev/socket | 含 gralloc、kgsl-0 | 两者消失 ✅ |
| TransportInfo BSSID / Net ID | 64:09… / 0 | 02:00… / -1 ✅ |

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
| ro.build.flavor | redfin-user | — | redfin-user | ✅ |
| ro.build.date | 2023-09-02 01:48:29 UTC | — | ★ 与 9 月构建相符 | ⚠️ |
| ro.build.version.security_patch | 2023-09-01 | 2026-08-01 | ★ 2023-09-01（该版本特例） | ✅ |
| ro.product.system.device / name / model | redfin / redfin / Pixel 5 | ossi ×3 | ★ redfin / redfin / Pixel 5 | ✅ |
| ro.product.first_api_level | 30 | 33 | ★ 30 | ✅ |
| ro.vendor.api_level | 30 | 31 | ★ 30（预期） | ✅ |
| ro.board.api_level | 空 | 31 | 未核对 | ⚠️ |
| ro.soc.manufacturer / model | QTI / SM7250 | QTI / SM8475 | QTI / SM7250 | ✅ |
| ro.board.platform | lito | taro | lito | ✅ |
| ro.hardware | redfin | qcom | redfin | ✅ |
| ro.hardware.egl | **angle** | adreno | ★ adreno | ❌ |
| ro.hardware.vulkan | adreno | adreno | adreno | ✅ |
| ro.secure / ro.debuggable | 1 / 0 | 1 / 0 | 1 / 0 | ✅ |
| ro.boot.verifiedbootstate / flash.locked | green / 1 | green / 1 | green / 1 | ✅ |
| ro.boot.veritymode | enforcing | — | enforcing | ✅ |
| ro.revision / ro.boot.hardware.revision | MP1.0 / MP1.0 | 0 / 空 | ★ MP1.0 | ✅ |
| uname | `4.19.282-g8b2a6e3c1f0d-ab10577962 #1 SMP PREEMPT Mon Jul 31 19:57:23 UTC 2023` | 5.10.236-android12-9-o-g74d132f4467a | ★ `4.19.278-g…`（redfin 公开内核串） | ⚠️ 4.19.282 串未见于 redfin 固件，待核对 |

## 2. 显示与硬件参数

| 字段 | 子系统 | 真机 PHK110 | Pixel 5 参照 | 判定 |
|---|---|---|---|---|
| display | 1080x2340 dpi=440 xdpi=440 | 1240x2772 dpi=560 | 1080x2340 @440ppi | ✅ |
| display.mode | **90.0 fps**，alternativeRefreshRates=[] | 60 fps，备选 90/120 | ★ 90Hz 面板 | ✅ |
| mem.total | 7,795,003,392（MemTotal 7,612,303 kB） | 15,957,581,824 | 8GB 机型，具体 MemTotal ⚠️ | ⚠️ |
| statfs.data | 118,648,471,552（≈110.5 GiB） | 492 GB | 128GB 机型，可用 ≈110GiB | ✅ |
| runtime.cpus | 8 | 8 | 8 | ✅ |
| cpufreq policies | policy0 / policy6 / policy7，1.8/2.2/2.4 GHz | policy0 / policy4 / policy7，2.0/2.75/3.19 GHz | ★ 同子系统（SM7250） | ✅ |
| cpuinfo part / features | 0x805×6 + 0x804×2；无 sha3/sm3/sm4/sha512/i8mm/bti | 0xd46/0xd47/0xd48；含上述扩展 | ★ 同子系统（lito） | ✅ |
| auxv HWCAP (AT_HWCAP) | 0x119fff | 0xefbfffff | 与 lito features 对应 | ✅ |

## 3. 传感器

- 子系统 HAL 共列出 15 个传感器；第三方应用可见 13 个（`dumpsys sensorservice`
  ML 里另有 Corrected Gyroscope、Gyroscope Bias(debug) 等）。
- 型号名：LSM6DSR（加速度/陀螺）、MMC56X3X（磁力计）、TMD3719（接近/光感）。
  这组名字公开资料更多见于 Pixel 6/7，Pixel 5 的实际器件型号 ⚠️ 待核对。
- **缺失项（相对 Pixel 5）**：
  - 气压计（`TYPE_PRESSURE`，Pixel 5 规格明确有）❌
  - 计步器 / 步数检测（`TYPE_STEP_COUNTER` / `TYPE_STEP_DETECTOR`）❌
  - 显著运动（`TYPE_SIGNIFICANT_MOTION`）❌
  - 各传感器的 wakeup 版本、gyro uncalibrated（type 16）❌
- Pixel 5 有后置指纹传感器；子系统没有（用户明确不要 NFC / 指纹，属有意不同）。

## 4. 相机

| 项 | 子系统 | 真机 PHK110 | Pixel 5 参照 | 判定 |
|---|---|---|---|---|
| camera.ids | 0,1 | 0,1 | ★ 0=后置 12.2MP，1=前置 8MP，另有后置 16MP 超广角 | ⚠️ 数量不同 |
| 实际出图 | JPEG 640×480 测试图（`ffffcbb5` / `ffd9c785`） | 未测（无 CAMERA 权限） | 12.2MP / 16MP / 8MP | ⚠️ 真机不可比；子系统为测试图 ❌ |
| features | camera.full/raw/manual/autofocus/flash/front | 同左 + NFC/指纹等 | 同左 | ⚠️ full/raw 与 640×480 测试面不匹配 |

## 5. 网络

| 字段 | 子系统 | 真机 PHK110 | Pixel 5 参照 | 判定 |
|---|---|---|---|---|
| net.info | `1 WIFI CONNECTED` | `1 WIFI CONNECTED` | 已连 Wi-Fi 时必有活动网络 | ✅ |
| net.wifi / iface | true / wlan0 | true / wlan0 | true / wlan0 | ✅ |
| net.interfaces | `wlan0 lo` | `rmnet_data0 dummy0 wlan0 rmnet_data1 lo vgate0 ifb0 ifb2 ifb1` | 无 SIM 时真机仍有 dummy0、rmnet_data* 等 | ❌ 接口过少 |
| wifi.enabled / sticky | true / 3 | true / 3 | true / 3 | ✅ |
| wifi.ssid | `"Xiaomi_4F5F"` | `<unknown ssid>` | 有定位权限时可显示 | ⚠️ 真机无定位权限，不可比 |
| wifi.scan | 5 个 AP | 空 | 有定位权限时应能扫描 | ⚠️ 不可比 |
| wifi.startScan | true，2s 后 updated=true | false | 有定位权限时应为 true | ⚠️ 不可比 |
| wifi.info BSSID / id | `64:09:80:72:4f:5f` / 0 | `02:00:00:00:00:00` / -1 | 依赖定位权限 | ⚠️ 真机不可比；与 TransportInfo 不一致 |
| net.caps TransportInfo | SSID `<unknown ssid>`、BSSID `02:00…`、Net ID -1 | 同左 | 同真机 | ✅ |

**Wi-Fi 两条路径不一致**：`getConnectionInfo()` 返回真实 BSSID/`id=0`，
`NetworkCapabilities` 的 TransportInfo 返回脱敏值。授权应用在真机上两条
路径都应是真实值（未授权则都脱敏），所以至少有一条不对；需在真机上授予
定位权限后复测才能定论。

## 6. 图形

| 字段 | 子系统 | 真机 PHK110 | Pixel 5 参照 | 判定 |
|---|---|---|---|---|
| gl.vendor / renderer | Qualcomm / Adreno (TM) 620 | Qualcomm / Adreno (TM) 730 | Qualcomm / Adreno 620 | ✅ |
| gl.version | `OpenGL ES 3.1 V@0502.0 …` | `OpenGL ES 3.2 V@0615.100 …` | ★ ES 3.2 | ❌ 3.1 vs 3.2 |
| gl.extensions | 118，angle=false | 104 | 未核对 | ⚠️ |
| egl.vendor | Android | Android | Android | ✅ |
| ro.hardware.egl | angle | adreno | ★ adreno | ❌ |

## 7. 文件系统、/proc 与权限面

| 项 | 子系统 | 真机 PHK110 | Pixel 5 参照 | 判定 |
|---|---|---|---|---|
| /proc/version | EACCES | EACCES | 现代 Android 同样拒绝 | ✅ 非差异 |
| /proc/sys/kernel/osrelease | EACCES | EACCES | 同上 | ✅ 非差异 |
| /proc/tty/drivers | 无 goldfish/virtio/ttyS | 无 | 无 | ✅ |
| /proc/self/maps 标记 | 空 | 大量 oplus（宿主机型） | Pixel 5 应为空 | ✅ |
| /proc/self/mountinfo 标记 | 空 | 大量 oplus（宿主机型） | 应为空 | ✅ |
| /proc/self/mounts 行数 | 66 | 218 | 真机约 200+ | ❌ 应用可见挂载过少 |
| /proc/self/cgroup | 仅 `0::/uid_…/pid_…` | v1 多行 + `0::` | ⚠️ A13 行为待核对 | ⚠️ |
| /proc/self/status | Seccomp=2 / filters=1 / NoNewPrivs=1 | Seccomp=2 / filters=1 / NoNewPrivs=0（探针可调试） | 非调试应用应为 1 | ✅ |
| 列 /dev | EACCES | EACCES | ★ 应用不可列 | ✅（本轮已修） |
| /dev/socket | 20 项，无 gralloc/kgsl-0；缺 vendor_wpa_wlan0/location/zygote_secondary | 55+ 项（含 OnePlus 私有）；有上述 3 个标准套接字 | 标准 AOSP 集含上述 3 个 | ⚠️ 假套接字已隐藏；仍缺标准项 |
| /run | unreadable | 不存在 | — | — |
| env.extra | 仅 TMPDIR | 大量 ColorOS 变量 | 少量 | — |
| `exists.*` 标记文件 | 全 false | 全 false | 全 false | ✅ |
| ls /system | 15 项 | 15 项 | — | ✅ |
| ls /system/bin | 414 项 | unreadable | 未核对 | ⚠️ |
| /vendor/etc/init | 14 项 | 110 项（宿主私有） | 未核对 | — |

## 8. 包

| 项 | 子系统 | 真机 PHK110 | Pixel 5 参照 | 判定 |
|---|---|---|---|---|
| 应用可见包数 | 140（shell 143） | 481 | 出厂含 GMS，显著更多 | ❌ |
| GMS | 无 | 有（ColorOS 自带 Google 套件） | ★ 有 | ❌（用户未要求补齐） |
| 应用可见的 LineageOS 包 | `org.lineageos.jelly`、`org.lineageos.aperture` | 无 | 应为 Chrome / Google Camera | ⚠️ 有意保留（浏览器/相机），但包名可被识别 |
| shell 可见的 LineageOS 包 | 上述 2 个 + `lineageos.platform`、`org.lineageos.lineagesettings`、`org.lineageos.lineageparts`、`org.lineageos.settingsconfig` | 无 | 无 | hide 规则对应用生效，shell 仍全见 |

## 9. 电话（无 SIM，有意保留）

| 字段 | 子系统 | 真机 PHK110 | Pixel 5 无 SIM 参照 | 判定 |
|---|---|---|---|---|
| gsm.sim.state / tel.simState | ABSENT / 1 | LOADED,ABSENT / 5（双卡且有卡） | ABSENT / 1 | ✅ |
| tel.phoneType | 1 (GSM) | 1 | 1 | ✅ |
| networkOperator / Name / CountryIso | 空 | 46001 / 中国联通 / cn | 空 | ✅ |
| activeSubs | 0 | SecurityException（无权限） | 0 | ⚠️ 不可比 |
| serviceState | null | SecurityException（无权限） | ★ 无 SIM 时预期 OUT_OF_SERVICE(1) | ⚠️ 待核对 |
| voiceCapable / smsCapable | true / true | true / true | true / true | ✅ |
| phoneCount | 1 | 2 | 双卡机型（nano+eSIM） | ⚠️ |
| gsm.version.ril-impl | Qualcomm RIL 1.0 | Qualcomm RIL 1.0 | Qualcomm RIL 1.0 | ✅ |
| init.svc.vendor.ril-daemon | 空 | 空 | 未核对 | ✅ 非差异 |

## 关键差异详述

### A. 真机探针没有运行时权限（方法学修正）

`pm grant` 在宿主上被 ColorOS 拒绝，所以真机侧的 SSID/BSSID/扫描、相机出图、
蓝牙名、`tel.serviceState` 等都是「无权限」结果。上一版报告据此判断的
「BSSID 未脱敏」需要修正：真机的 `02:00…` 是无权限脱敏，不能证明授权后
也会脱敏；子系统的两条 Wi-Fi 路径（TransportInfo 已脱敏、ConnectionInfo
未脱敏）自相矛盾，必须在真机上授予定位权限后复测才能定论。

### B. GL 与 EGL 库名

Renderer 已伪装成 Adreno 620，但：

- `gl.version` 仍是 `OpenGL ES 3.1`（ANGLE 上限），Pixel 5 是 ES 3.2；
- `ro.hardware.egl=angle`，Pixel 5 是 `adreno`。

### C. 传感器缺口

Pixel 5 规格明确带气压计；子系统没有 `TYPE_PRESSURE`。此外计步、显著运动、
gyro uncalibrated、各 wakeup 传感器也缺失。

### D. 相机是测试图

相机 HAL 能出图（0=后置、1=前置），但都是 640×480 的静态测试图，
`features` 却声明了 `camera.level.full` / `raw`。Pixel 5 是 12.2MP 主摄 +
16MP 超广角 + 8MP 前摄。

### E. 网络接口过少

应用枚举 `NetworkInterface.getNetworkInterfaces()` 时，子系统只有 `wlan0 lo`；
真机（即便无 SIM）也有 `dummy0`、`rmnet_data*`、`ifb*` 等。

### F. 应用可见的挂载数

子系统 66 行，真机 218 行；`/dev` 的权限语义本轮已修（应用不再能枚举）。

### G. /dev/socket 缺标准套接字

假套接字 gralloc/kgsl-0 已隐藏；但真机上常见的 `vendor_wpa_wlan0`、
`location`、`zygote_secondary` 在子系统里没有。

### H. 包与生态

140 个包、无 GMS；Pixel 5 出厂自带 GMS。`org.lineageos.jelly` /
`aperture` 对应用可见（有意保留浏览器/相机）。

### I. 待核对

- `uname` 的 `4.19.282-g…` 串未见于 redfin 公开固件（公开 A13 是 4.19.278-g…）
- 无 SIM 时 `tel.serviceState` 是否为 `null`（真机需授权后复测）
- `phoneCount=1` 与 Pixel 5 双卡（nano+eSIM）的关系
- `MemTotal` 的具体值
- `Build.HOST`、`ro.build.date` 与官方构建的对应关系

## 附：已知的探针盲区

- `<<MARK` 只匹配「值」，`pkg.<包名>` 的包名在 key 上；`org.lineageos.jelly` /
  `aperture` 就是这样被漏掉的。
- 探针没有调用 `CameraCharacteristics`、`Display.getSupportedModes()`、
  `getSupportedModemCount()` 等更深接口，以上部分「待核对」项需要扩探针才能定论。
- 真机侧 `pm grant` 不可用，权限相关字段只能等真机 UI 授权或换测试设备。

## 复现方法

子系统（在宿主机上执行，自动安装/卸载探针）：

```sh
scripts/probe/run.sh
```

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

- Google Pixel 工厂镜像/OTA（TQ3A.230901.001.C2 的 bootloader、radio）：
  <https://developers.google.com/android/images>
- 2023-09 Pixel 更新与 `.C2`（Verizon）版本：
  <https://9to5google.com/2023/09/18/android-13-september-pixel-update/>
- Pixel 5 规格（90Hz、12.2MP+16MP、8MP 前摄、气压计）：
  <https://devicehelp.optus.com.au/google/pixel-5-android-11-0/specifications>
- Pixel 5 `ro.product.first_api_level=30`：
  <https://xdaforums.com/t/pixelflasher-for-google-pixel-5-support-thread.4419577/page-10>
- redfin 公开内核串（4.19.278-g…）：
  <https://github.com/7waynelee/pixel5-kernelsu-next>
- 2023-09 构建的 security_patch 仍为 2023-09-01（该版本特例）：
  <https://www.nobsbitcoin.com/grapheneos-v2023091800/>

## 附录 A：子系统探针原始输出（2026-11-24 18:22，提交 580583c）

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
os.version=4.19.282-g8b2a6e3c1f0d-ab10577962
prop.waydroid.display_width=
prop.waydroid.stub_sensors_hal=
prop.ro.lineage.version=
prop.ro.modversion=
prop.ro.lineagelegal.url=
prop.sys.lineage_settings_system_version=
prop.ro.board.platform=lito
prop.ro.hardware=redfin
prop.ro.hardware.egl=angle
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
net.info=1 WIFI CONNECTED
net.caps=transportInfo=SSID: <unknown ssid>, BSSID: 02:00:00:00:00:00, MAC: 02:00:00:00:00:00, IP: /192.168.31.105, Security type: 2, Supplicant state: COMPLETED, Wi-Fi standard: 5, RSSI: -52, Link speed: 866Mbps, Tx Link speed: 866Mbps, Max Supported Tx Link speed: 866Mbps, Rx Link speed: 780Mbps, Max Supported Rx Link speed: 866Mbps, Frequency: 5180MHz, Net ID: -1, Metered hint: false, score: 0, isUsable: true, CarrierMerged: false, SubscriptionId: -1, IsPrimary: -1, Trusted: false, Restricted: false, Ephemeral: false, OEM paid: false, OEM private: false, OSU AP: false, FQDN: <none>, Provider friendly name: <none>, Requesting package name: <none><none>MLO Information: , AP MLD Address: <none>, AP MLO Link Id: <none>, AP MLO Affiliated links: <none> specifier=null
net.wifi=true
net.ethernet=false
net.cellular=false
net.iface=wlan0
net.interfaces=wlan0 lo 
wifi.enabled=true
wifi.ssid="Xiaomi_4F5F"
wifi.scan=Xiaomi_4F5F/-52 Xiaomi_4F5F/-47 ChinaNet-5619/-84 TP-LINK_0B91/-72 HUAWEI-DD10/-72 ChinaUnicom-918C/-74 
wifi.sticky=3
wifi.startScan=true broadcast=updated=true
wifi.info=64:09:80:72:4f:5f rssi=-52 866Mbps 5180MHz std=5 id=0 COMPLETED ip=1763682496 mac=02:00:00:00:00:00
sensor.1=LSM6DSR Accelerometer | STMicroelectronics
sensor.4=LSM6DSR Gyroscope | STMicroelectronics
sensor.2=MMC56X3X Magnetometer | STMicroelectronics
sensor.8=TMD3719 Proximity | STMicroelectronics
sensor.5=TMD3719 Ambient Light | STMicroelectronics
sensor.14=MMC56X3X Magnetometer Uncalibrated | STMicroelectronics
sensor.35=LSM6DSR Accelerometer Uncalibrated | STMicroelectronics
sensor.15=Game Rotation Vector Sensor | AOSP
sensor.20=GeoMag Rotation Vector Sensor | AOSP
sensor.9=Gravity Sensor | AOSP
sensor.10=Linear Acceleration Sensor | AOSP
sensor.11=Rotation Vector Sensor | AOSP
sensor.3=Orientation Sensor | AOSP
gl.vendor=Qualcomm
gl.renderer=Adreno (TM) 620
gl.version=OpenGL ES 3.1 V@0502.0 (GIT@7d8a5d1cd0, I9a9f0b7e8b, 1683677478) (Date:05/09/23)
gl.extensions=118 num=118 angle=false
egl.vendor=Android
proc.version=ERR FileNotFoundException: /proc/version: open failed: EACCES (Permission denied)
proc.cpuinfo.hardware=CPU part	: 0x805 ; CPU part	: 0x805 ; CPU part	: 0x805 ; CPU part	: 0x805 ; CPU part	: 0x805 ; CPU part	: 0x805 ; CPU part	: 0x804 ; CPU part	: 0x804 ; Hardware	: Qualcomm Technologies, Inc LITO ; 
proc.mounts.lines=66
proc.mounts.0=none /data/misc/profiles/ref/dev.probe.env f2fs rw,relatime 0 0
proc.mounts.1=none /data/misc/profiles/cur/0/dev.probe.env f2fs rw,relatime 0 0
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
ls./vendor=11 entries 
ls./vendor/bin/hw=12 entries 
ls./vendor/etc/init=14 entries 
ls./vendor/lib64=108 entries 
ls./vendor/lib64/hw=32 entries 
ls./product/etc=11 entries 
ls./product/etc/init=1 entries 
ls./product/etc/permissions=4 entries 
ls./system_ext/etc/init=1 entries 
ls./system_ext/etc/permissions=10 entries 
ls./vendor/etc/permissions=25 entries 
ls./vendor/etc/vintf/manifest=6 entries 
dir./run=unreadable
dir./run/user/1000=unreadable
dir./dev/socket=adbd audioserver dnsproxyd fwmarkd lmkd logd logdr logdw mdns mdnsd prng_seeder property_service statsdw tombstoned_crash tombstoned_intercept tombstoned_java_trace traced_consumer traced_producer usap_pool_primary zygote
dir./dev=unreadable
env.extra=TMPDIR=/data/user/0/dev.probe.env/cache 
exists./vendor/waydroid.prop=false
exists./system/etc/init/init.waydroid.rc=false
exists./product/etc/vine=false
exists./system/bin/vine_lmkd=false
exists./vendor/lib64/libcuttlefish_fs.so=false
exists./system/addon.d/50-lineage.sh=false
proc.self.maps.marks=
display=1080x2340 dpi=440 xdpi=440.0
display.mode={id=1, width=1080, height=2340, fps=90.0, alternativeRefreshRates=[]}
mem.total=7795003392
proc.meminfo=MemTotal:        7612303 kB ; MemFree:           70386 kB ; MemAvailable:    1863566 kB ; 
statfs.data=118648471552 free=81026166784
runtime.cpus=8
cpufreq=0:1804800/300000 rel=0 1 2 3 4 5 midr=0x00000000517f805e; 1:1804800/300000 rel=0 1 2 3 4 5 midr=0x00000000517f805e; 2:1804800/300000 rel=0 1 2 3 4 5 midr=0x00000000517f805e; 3:1804800/300000 rel=0 1 2 3 4 5 midr=0x00000000517f805e; 4:1804800/300000 rel=0 1 2 3 4 5 midr=0x00000000517f805e; 5:1804800/300000 rel=0 1 2 3 4 5 midr=0x00000000517f805e; 6:2208000/652800 rel=6 midr=0x0000000051ff804e; 7:2400000/806400 rel=7 midr=0x0000000051ff804e; 
cpufreq.policies=policy0 policy6 policy7
sys.cpu.possible=0-7 online=0-7
proc.cpuinfo.full=processor	: 0|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp|CPU implementer	: 0x51|CPU architecture: 8|CPU variant	: 0x7|CPU part	: 0x805|CPU revision	: 14||processor	: 1|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp|CPU implementer	: 0x51|CPU architecture: 8|CPU variant	: 0x7|CPU part	: 0x805|CPU revision	: 14||processor	: 2|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp|CPU implementer	: 0x51|CPU architecture: 8|CPU variant	: 0x7|CPU part	: 0x805|CPU revision	: 14||processor	: 3|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp|CPU implementer	: 0x51|CPU architecture: 8|CPU variant	: 0x7|CPU part	: 0x805|CPU revision	: 14||processor	: 4|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp|CPU implementer	: 0x51|CPU architecture: 8|CPU variant	: 0x7|CPU part	: 0x805|CPU revision	: 14||processor	: 5|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp|CPU implementer	: 0x51|CPU architecture: 8|CPU variant	: 0x7|CPU part	: 0x805|CPU revision	: 14||processor	: 6|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp|CPU implementer	: 0x51|CPU architecture: 8|CPU variant	: 0xf|CPU part	: 0x804|CPU revision	: 14||processor	: 7|BogoMIPS	: 38.40|Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp|CPU implementer	: 0x51|CPU architecture: 8|CPU variant	: 0xf|CPU part	: 0x804|CPU revision	: 14||Hardware	: Qualcomm Technologies, Inc LITO|
uname.osrelease=ERR FileNotFoundException: /proc/sys/kernel/osrelease: open failed: EACCES (Permission denied)
uname=4.19.282-g8b2a6e3c1f0d-ab10577962 | #1 SMP PREEMPT Mon Jul 31 19:57:23 UTC 2023
proc.self.cgroup=0::/uid_10117/pid_15901|
proc.self.mountinfo.marks=
proc.self.status=TracerPid:	0 ; NoNewPrivs:	1 ; Seccomp:	2 ; Seccomp_filters:	1 ; Cpus_allowed_list:	0-7 ; 
camera.ids=0,1
camera.capture.0=jpeg 11583 640x480 px(320,240)=ffffcbb5
camera.capture.1=jpeg 11452 640x480 px(320,240)=ffd9c785
features.hw=android.hardware.bluetooth android.hardware.camera.autofocus android.hardware.camera.flash android.hardware.camera.front android.hardware.bluetooth_le android.hardware.camera android.hardware.camera.capability.manual_post_processing android.hardware.camera.any android.hardware.camera.capability.raw android.hardware.camera.capability.manual_sensor android.hardware.camera.level.full android.sofware.nfc.beam 
bt=false name=Pixel 5 addr=02:00:00:00:00:00
battery=level=64 plugged=2 status=2 volt=3962 temp=262 tech=Li-ion
auxv=3=78e2593040 9=78e2595000 16=119fff 
Build.BOOTLOADER=r3-0.5-9825705
prop.revision=MP1.0 hw=MP1.0 bootmode=unknown
gl.egl.prop=angle
pkg.org.lineageos.jelly=/system/product/app/Jelly/Jelly.apk
pkg.org.lineageos.aperture=/system/product/app/Aperture/Aperture.apk
pkg.count=140
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
