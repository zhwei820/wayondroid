# 编译与使用

## 编译

### 构建主机

**必须是 Linux x86_64。** `tawcroot/build.sh` 和 `scripts/gpu/*.sh` 写死了 NDK 的
`toolchains/llvm/prebuilt/linux-x86_64` 工具链，Gradle 的 `preBuild` 会调用
`tawcroot/build.sh`，所以在 macOS / Windows 上 `./gradlew :app:assembleDebug` 会在
`buildTawcroot*` 任务报 `NDK toolchain missing`。

macOS 上可用 Linux x86_64 虚拟机，或 CI 用的镜像（Apple Silicon 需
`--platform linux/amd64`，走模拟，较慢）：

```sh
docker run --rm -it --platform linux/amd64 -v "$PWD":/src -w /src eclipse-temurin:17-jdk-jammy bash
```

网络有 HTTPS 中间人代理（如 Zscaler）时，容器里的 curl 和 Gradle 会报 `SSL certificate problem` /
`PKIX path building failed`。把代理的根证书从 macOS 钥匙串导出，挂进容器；temurin 镜像在
`USE_SYSTEM_CA_CERTS=1` 时会把 `/certificates/*.crt` 同时加入系统证书库和 JDK 的 cacerts：

```sh
mkdir -p ~/docker-certs
security find-certificate -a -c "Zscaler Root CA" -p /Library/Keychains/System.keychain > ~/docker-certs/zscaler.crt
docker run --rm -it --platform linux/amd64 \
    -e USE_SYSTEM_CA_CERTS=1 -v ~/docker-certs:/certificates:ro \
    -v "$PWD":/src -w /src eclipse-temurin:17-jdk-jammy bash
```


colima stop                                                                                                                                                                                                    
colima start --cpu 6 --memory 10


docker run --rm -it --platform linux/amd64 -e USE_SYSTEM_CA_CERTS=1 -e JAVA_TOOL_OPTIONS="-Djavax.net.ssl.trustStore=/opt/java/openjdk/lib/security/cacerts -Djavax.net.ssl.trustStorePassword=changeit" -e ANDROID_HOME=/root/Android/Sdk -v ~/docker-certs:/certificates:ro -v wayondroid-sdk:/root/Android -v wayondroid-gradle:/root/.gradle -v "$PWD":/src -w /src eclipse-temurin:17-jdk-jammy bash


注意：仓库根目录的 `local.properties` 里如果有宿主机的 `sdk.dir`（如 `/Users/...`），它优先于
`ANDROID_HOME`，在容器里会找不到 SDK。容器内构建前把它改成容器里的 SDK 路径，或临时挪开。

### 依赖

| 依赖 | 说明 |
|---|---|
| JDK 17 | CI 使用 `eclipse-temurin:17-jdk-jammy` |
| Android SDK | `compileSdk = 37`；`ANDROID_HOME` 指向 SDK，或在 `local.properties` 写 `sdk.dir=` |
| Android NDK | `28.2.13676358`（AGP 9.4.1 的默认版本），装在 `$ANDROID_HOME/ndk/` 下 |
| CMake 3.22.1 | 通过 SDK Manager 安装（`cmake;3.22.1`） |

全新的 Linux 环境可以用 CI 的脚本装 SDK 组件：

```sh
                                                                                                                   
  sed -i 's#^sdk.dir=.*#sdk.dir=/root/Android/Sdk#' local.properties                                                  
  ANDROID_SDK_NATIVE=true ANDROID_NDK_VERSION=28.2.13676358 scripts/ci/setup-android-sdk.sh
                             
```

不传 `ANDROID_NDK_VERSION` 时脚本不装 NDK，Gradle 会在构建时自动下载 AGP 的默认版本（CI 就是这样）。
但 `scripts/gpu/build-gpu.sh` 在 Gradle 之前运行，取的是 `$ANDROID_HOME/ndk/` 下版本号最大的那个，
所以要构建 GPU 组件就得预先装好；升级 AGP 后这个版本号要跟着改。

### 构建 APK

```sh
  ./gradlew :app:assembleDebug --no-watch-fs                                                                                                                                                                     

```

产物：`app/build/outputs/apk/debug/app-debug.apk`，包名 `com.hexadecinull.vineos.debug`。

Gradle 会先构建 tawcroot 和 binderd，放进 `app/src/main/jniLibs/<abi>/`
（`libtawcroot.so`、`libbinderd.so`），只有 `arm64-v8a` 和 `x86_64` 有；rootless 引擎在
32 位 ABI 上不可用。

Release 构建：

```sh
#./gradlew :app:assembleRelease --no-watch-fs
```


export JAVA_TOOL_OPTIONS="-Djavax.net.ssl.trustStore=/opt/java/openjdk/lib/security/cacerts -Djavax.net.ssl.trustStorePassword=changeit"                                                                       
./gradlew --stop                                                                                                                                                                                               
./gradlew :app:assembleRelease


根目录有 `keystore.properties`（`storeFile`、`storePassword`、`keyAlias`、`keyPassword`）
时签名，没有则产出未签名的 APK。

### GPU 加速组件（可选）

不构建也能用，只是不能开 GPU 加速：开了的话启动会报 `this build has no GPU acceleration`。
需要时在 `assembleDebug` **之前**运行：

```sh
scripts/gpu/build-gpu.sh          # 加 --clean 从头构建
```

额外依赖：`meson`、`ninja`、`pkg-config`、`cargo`（需要 Rust 的 `aarch64-linux-android`
target）。只产出 `arm64-v8a`：`libgfxstream_backend.so`、`libvine_kumquat.so`、
`libvine_vk_gfxstream.so`。

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

手机要求：arm64，rootless 引擎只在 Android 17 上测过。零拷贝显示需要 Android 10+。

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
   **Create Instance**。存储要预留约 ROM 两倍的空间。
3. **启动**：在实例详情点 **Launch**。首次启动要解压镜像并编译所有 App，需要几分钟。
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
- 启动卡住：停止后重新启动。
- 画面异常或 App 崩溃：先关掉 GPU 加速。
- 32 位 App 无法运行，guest 只支持 64 位。

更多：[overview.md](overview.md)、[rootless.md](rootless.md)、[rootless-boot.md](rootless-boot.md)。
