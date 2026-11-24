# 编译与使用

## 编译

### 构建主机

**必须是 Linux x86_64。** `tawcroot/build.sh` 和 `scripts/gpu/*.sh` 写死了 NDK 的
`toolchains/llvm/prebuilt/linux-x86_64` 工具链，Gradle 的 `preBuild` 会调用
`tawcroot/build.sh`，所以在 macOS / Windows 上 `./gradlew :app:assembleDebug` 会在
`buildTawcroot*` 任务报 `NDK toolchain missing`。

### macOS：在 Docker 容器里构建

Apple Silicon 上用 `--platform linux/amd64` 跑 x86_64 镜像，走模拟，较慢。

**虚拟机资源**：Gradle 守护进程要 4 GB 堆（`gradle.properties` 的 `-Xmx4096m`），加上 Kotlin
编译进程和模拟开销，虚拟机内存不够时守护进程会被直接杀掉（`Gradle build daemon disappeared
unexpectedly`）。用 Colima 时至少给 10 GB：

```sh
colima stop
colima start --cpu 6 --memory 10     # 参数写进 ~/.colima/default/colima.yaml，以后直接 colima start
```

**HTTPS 中间人代理（如 Zscaler）**：容器不信任代理的根证书，curl 报 `SSL certificate problem`，
Gradle 报 `PKIX path building failed` 或 `Plugin ... was not found`。把根证书从 macOS 钥匙串导出：

```sh
mkdir -p ~/docker-certs
security find-certificate -a -c "Zscaler Root CA" -p /Library/Keychains/System.keychain > ~/docker-certs/zscaler.crt
```

启动容器（在仓库根目录执行）：

```sh
docker run --rm -it --platform linux/amd64 -e USE_SYSTEM_CA_CERTS=1 -e JAVA_TOOL_OPTIONS="-Djavax.net.ssl.trustStore=/opt/java/openjdk/lib/security/cacerts -Djavax.net.ssl.trustStorePassword=changeit" -e ANDROID_HOME=/root/Android/Sdk -v ~/docker-certs:/certificates:ro -v wayondroid-sdk:/root/Android -v wayondroid-gradle:/root/.gradle -v wayondroid-android-home:/root/.android -v "$PWD":/src -w /src eclipse-temurin:17-jdk-jammy bash

                                                   
docker run --rm -it --platform linux/amd64 -e USE_SYSTEM_CA_CERTS=1 -e JAVA_TOOL_OPTIONS="-Djavax.net.ssl.trustStore=/opt/java/openjdk/lib/security/cacerts -Djavax.net.ssl.trustStorePassword=changeit" -e    ANDROID_HOME=/root/Android/Sdk -v ~/docker-certs:/certificates:ro -v wayondroid-sdk:/root/Android -v wayondroid-gradle:/root/.gradle -v wayondroid-android-home:/root/.android -v wayondroid-cargo:/root/.cargo -v wayondroid-rustup:/root/.rustup -v      "$PWD":/src -w /src eclipse-temurin:17-jdk-jammy bash                        
```

- `USE_SYSTEM_CA_CERTS=1`：temurin 镜像启动时把 `/certificates/*.crt` 加进系统证书库和 JDK 17 的 cacerts。
- `JAVA_TOOL_OPTIONS`：`gradle/gradle-daemon-jvm.properties` 要求守护进程跑在 JDK 25 上，Gradle
  会自己下载一个，它的证书库里没有代理证书；这个变量让所有 JVM 改用 JDK 17 那份已导入证书的 cacerts。
- `wayondroid-sdk`、`wayondroid-gradle` 两个 volume 保存 SDK 和 Gradle 缓存，`--rm` 删掉容器后不用重新下载。
- `wayondroid-android-home` 保存 debug 签名密钥（`/root/.android/debug.keystore`）。不挂的话每个新容器都生成新密钥，
  编出的包签名变了，手机上无法覆盖安装（`INSTALL_FAILED_UPDATE_INCOMPATIBLE`），只能卸载重装、丢掉实例。
- 命令要写成一行，或确认每行末尾的 `\` 后面没有空格，否则会报 `invalid reference format`。

### 依赖

| 依赖 | 说明 |
|---|---|
| JDK 17 | CI 使用 `eclipse-temurin:17-jdk-jammy` |
| Android SDK | `compileSdk = 37`；`ANDROID_HOME` 指向 SDK，或在 `local.properties` 写 `sdk.dir=` |
| Android NDK | `28.2.13676358`（AGP 9.4.1 的默认版本），装在 `$ANDROID_HOME/ndk/` 下 |
| CMake 3.22.1 | 通过 SDK Manager 安装（`cmake;3.22.1`） |

容器里首次安装 SDK 组件：

```sh
sed -i 's#^sdk.dir=.*#sdk.dir=/root/Android/Sdk#' local.properties
ANDROID_SDK_NATIVE=true ANDROID_NDK_VERSION=28.2.13676358 scripts/ci/setup-android-sdk.sh
```

`local.properties` 的 `sdk.dir` 优先于 `ANDROID_HOME`，留着宿主机路径（`/Users/...`）容器里会找不到
SDK。仓库是挂载进容器的，这一改宿主机上也生效，回到 Android Studio 前要改回去。

不传 `ANDROID_NDK_VERSION` 时脚本不装 NDK，Gradle 会在构建时自动下载 AGP 的默认版本（CI 就是这样）。
但 `scripts/gpu/build-gpu.sh` 在 Gradle 之前运行，取的是 `$ANDROID_HOME/ndk/` 下版本号最大的那个，
所以要构建 GPU 组件就得预先装好；升级 AGP 后这个版本号要跟着改。

### 构建 APK

```sh
./gradlew :app:assembleDebug --no-watch-fs
```

`--no-watch-fs`：x86_64 模拟下 Gradle 的文件监听会报 `Couldn't poll for events`，关掉不影响构建。

产物：`app/build/outputs/apk/debug/app-debug.apk`，包名 `com.hexadecinull.vineos.debug`。

Gradle 会先构建 tawcroot 和 binderd，放进 `app/src/main/jniLibs/<abi>/`
（`libtawcroot.so`、`libbinderd.so`），只有 `arm64-v8a` 和 `x86_64` 有；rootless 引擎在
32 位 ABI 上不可用。

Release 构建：

```sh
#./gradlew :app:assembleRelease --no-watch-fs
```

根目录有 `keystore.properties`（`storeFile`、`storePassword`、`keyAlias`、`keyPassword`）
时签名，没有则产出 `app-release-unsigned.apk`，不能直接安装。Release 还要跑 R8 压缩混淆，
更吃内存和时间；自己用编 debug 即可。

### GPU 加速组件（可选）

不构建也能用，只是不能开 GPU 加速：开了的话启动会报 `this build has no GPU acceleration`。
不开 GPU 时 guest 用 `gralloc.default` 分配缓冲区，它只支持 RGB 格式，视频解码要的 YV12 /
YCbCr_420_888 分配失败（logcat 里 `GraphicBufferAllocator: Failed to allocate ... format
842094169`），播视频的 App 会卡在重试里；GPU 模式的 gralloc（`gralloc_gpu.c`）支持这两种格式。

`scripts/gpu/build-gpu.sh` 产出到 `app/src/main/jniLibs/arm64-v8a/`，只有 arm64：

| 产物 | 作用 | 源码 |
|---|---|---|
| `libgfxstream_backend.so`、`libc++_shared.so` | 宿主侧渲染器，在手机 GPU 上执行 guest 的 Vulkan 命令 | gfxstream |
| `libvine_vk_gfxstream.so` | guest 的 Vulkan 驱动，复制进 guest 的 `/vendor` | Mesa |
| `libvine_kumquat.so` | guest 与宿主渲染器之间的 GPU 服务 | `gpu/kumquat` + rutabaga_gfx（Rust） |

源码按 `deps/deps.list` 固定的提交克隆到 `deps/`，打上 `deps/*-patches/` 的补丁。源码加构建产物
要几个 GB 磁盘。

在上面的 Docker 容器里构建时，`docker run` 再加两个 volume 保存 Rust 工具链：
`-v wayondroid-cargo:/root/.cargo -v wayondroid-rustup:/root/.rustup`。容器内：

```sh

sed -i 's#^sdk.dir=.*#sdk.dir=/root/Android/Sdk#' local.properties
ANDROID_SDK_NATIVE=true ANDROID_NDK_VERSION=28.2.13676358 scripts/ci/setup-android-sdk.sh

# 构建工具（每个新容器都要装）。Mesa 25.3 要 meson >= 1.4，jammy 的 apt 版本是 0.61
apt-get update && apt-get install -y git curl pkg-config ninja-build python3-pip
pip3 install 'meson>=1.4' mako packaging pyyaml

# Rust（装一次，存在 volume 里）；两个变量让 rustup / cargo 用系统证书库（代理证书在里面）
export RUSTUP_USE_CURL=1 CARGO_HTTP_CAINFO=/etc/ssl/certs/ca-certificates.crt
curl https://sh.rustup.rs -sSf | sh -s -- -y --profile minimal
. ~/.cargo/env
rustup target add aarch64-linux-android

# 仓库是挂载进来的，属主与容器的 root 不同，git 默认拒绝操作
git config --global --add safe.directory '*'

scripts/gpu/build-gpu.sh          # 加 --clean 从头构建；首次要克隆并编译，模拟下可能一小时以上
scripts/sensors/fetch-hal.py      # guest 的传感器 HAL 和 RIL（模拟器镜像里的预编译件，需要 debugfs）；不跑则 guest 没有传感器和 SIM 卡
scripts/radio/build-overlay.sh    # 让 guest framework 使用 RIL 的 overlay（需要 SDK build-tools）
./gradlew :app:assembleDebug --no-watch-fs
```

GPU 组件必须在 `assembleDebug` **之前**构建。装上新包后，在 App 的 Settings 打开
「GPU acceleration (experimental)」，停止并重新启动实例（启动时才生效）。GPU 加速是实验性的，
只在 Mali（Galaxy S24）上测过；画面异常或崩溃时关掉开关即回到软件渲染。

### 测试

```sh
tawcroot/test.sh --host                          # tawcroot 运行时
app/src/main/cpp/tests/native/run_rootless_tests.sh   # 引擎，另有 wayland / display / pulse
./gradlew :app:testDebugUnitTest                 # App 单元测试
```

## 安装

```sh
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

手机要求：arm64。rootless 引擎在 Galaxy S24（Android 17）和骁龙 8+ Gen 1 的 ColorOS 机型
（Android 16）上启动过。零拷贝显示需要 Android 10+。

### 关闭子进程限制（Android 12+ 必需）

Android 12 起，系统把 App 自己创建的子进程算作"幽灵进程"，全机超过 32 个就从最早的开始杀。
guest 开机到 system_server 阶段就有 60 多个进程，引擎核心进程会被先杀掉，guest 随之整体退出；
系统日志里是 `Killing PhantomProcessRecord ... Trimming phantom processes`。每台手机执行一次：

```sh
adb shell settings put global settings_enable_monitor_phantom_procs false
adb shell device_config set_sync_disabled_for_tests persistent
adb shell device_config put activity_manager max_phantom_processes 2147483647
```

第一条等同于「开发者选项 → 停用子进程限制」（Android 14+）；后两条防止系统下发的配置把上限改回去。
重启后仍有效。恢复：第一条改成 `true`，再执行 `adb shell device_config set_sync_disabled_for_tests none`。

## 准备 ROM

1. 从 Waydroid OTA 下载 `waydroid_arm64_only` 的 system 和 vendor 镜像（JSON 里有 URL 和 sha256）：
   - system：`https://ota.waydro.id/system/lineage/waydroid_arm64_only/VANILLA.json`
     （或 `GAPPS.json`，带 Google 应用）
   - vendor：`https://ota.waydro.id/vendor/waydroid_arm64_only/MAINLINE.json`
2. 解压到同一个目录，得到 `system.img` 和 `vendor.img`。
3. 打包：

   ```sh
   python3 rom/package_vrom.py --kind waydroid IMAGES_DIR waydroid-lineage20.vrom \
       --name "Waydroid LineageOS 20" --android-version 13 --api-level 33
   ```
4. 把 `.vrom` 传到手机上：`adb push waydroid-lineage20.vrom /sdcard/Download/`。

## 使用

1. **导入 ROM**：打开 **ROMs** 标签页，点顶栏的导入按钮，选 `.vrom` 文件。
2. **创建实例**：在 **Instances** 首页点新建按钮，或在 ROM 详情页直接新建（**New Instance**），设置名称、RAM、存储大小，点
   **Create Instance**。存储要预留约 ROM 两倍的空间；RAM 建议 3～4 GB，GAPPS 只给 1 GB 会频繁杀后台。
3. **启动**：在实例详情点 **Launch**。首次启动要解压镜像并编译所有 App，需要几分钟。GAPPS 镜像首次开机先进 Google 开机向导。
4. **停止**：实例详情点 **Stop**。

常用设置：

| 位置 | 项 | 说明 |
|---|---|---|
| Settings | GPU acceleration (experimental) | 用手机 GPU 渲染，下次启动生效；需要构建了 GPU 组件 |
| Settings | Guest internet access | guest 联网 |
| Settings | Default RAM | 新实例的默认内存 |
| 实例详情 → Display | Frame rate、Resolution | 下次启动生效 |

## 排查问题

- 实例详情点 **Show diagnostics**，可看到各组件日志的末尾、kmsg 和 logcat。
- 完整日志在实例目录的 `rootless/logs/`：`prepare`、`init`、`binderd`、`vine_wl`、`vine_pulse`、`vine_kumquat`。
  debug 包可以用 `adb shell run-as com.hexadecinull.vineos.debug` 进入 App 数据目录读取。
- guest 的崩溃记录在 `files/instances/<id>/rootless/data/tombstones/`，guest 内核日志在
  `rootless/rootfs/dev/kmsg`（以 NUL 分隔）。
- guest 启动几十秒后整体消失、没有崩溃记录：先查系统日志里有没有 `PhantomProcessRecord`，见[关闭子进程限制](#关闭子进程限制android-12-必需)。
- 启动卡住：停止后重新启动。
- 画面异常或 App 崩溃：先关掉 GPU 加速。
- 32 位 App 无法运行，guest 只支持 64 位。

更多：[overview.md](overview.md)、[rootless.md](rootless.md)、[rootless-boot.md](rootless-boot.md)。
