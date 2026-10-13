# 依赖说明（DEPENDENCIES）

本文说明 rgaconv 插件 / kmswake 工具需要**哪些依赖**、**怎么安装**，分运行（安装 deb）与编译（源码构建）两种场景。

## 一、环境要求

| 项目 | 要求 |
| --- | --- |
| CPU 架构 | aarch64（arm64） |
| 平台 | Rockchip RK356x / RK3588（需要 RGA 硬件） |
| 系统 | Debian 11 (bullseye)（已验证）；其他发行版需自行满足下列依赖 |
| 内核 | 带 DRM/KMS + Rockchip RGA 驱动（`/dev/rga`、`/dev/dri/*`） |

## 二、运行时依赖（安装 deb 需要）

`gstreamer1.0-rgaconv_1.0-1_arm64.deb` 的 `Depends` 已声明：

| 依赖包 | 说明 | 已验证版本 |
| --- | --- | --- |
| libgstreamer1.0-0 (>= 1.14) | GStreamer 核心运行库 | 1.18.5 |
| libgstreamer-plugins-base1.0-0 | GStreamer base 运行库（gstvideo / GstBaseTransform） | 1.18.5 |
| librga2 | Rockchip RGA 用户态库（im2d / imcvtcolor） | 2.2.0-1 |

安装命令（前两个一般系统已带）：

```bash
sudo apt update
sudo apt install -y libgstreamer1.0-0 libgstreamer-plugins-base1.0-0
```

### librga 从哪来

Rockchip 只提供预编译包（Debian 源里没有），二选一：

1) 板卡镜像/SDK 自带 deb（推荐，与内核驱动配套）：

```bash
sudo apt install -y ./librga2_2.2.0-1_arm64.deb ./librga-dev_2.2.0-1_arm64.deb
```

2) 源码编译（https://github.com/airockchip/librga ）：

```bash
git clone https://github.com/airockchip/librga.git
cd librga && cmake -S . -B build && cmake --build build -j4
sudo cmake --install build
```

注意：编译版需保证 `liblibrga.so` 进入默认库路径（如 `/usr/lib/aarch64-linux-gnu/`）并可被 `ldconfig` 找到。

## 三、编译依赖（从源码构建时需要）

= 运行时依赖 + 以下开发包：

| 依赖包 | 说明 | 已验证版本 |
| --- | --- | --- |
| gcc | 编译器 | 10.2.1（Debian 11） |
| pkg-config | 探测 GStreamer/librga 编译参数 | - |
| libgstreamer1.0-dev | GStreamer 开发头文件 | 1.18.5 |
| libgstreamer-plugins-base1.0-dev | gstbase/gstvideo 头文件 | 1.18.5 |
| librga-dev | RGA 头文件（rga.h / im2d.h / im2d_single.h 等） | 2.2.0-1 |
| libdrm-dev | **仅 kmswake 需要**（xf86drm.h / xf86drmMode.h） | - |

安装命令：

```bash
sudo apt update
sudo apt install -y gcc pkg-config \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libdrm-dev
```

librga-dev 同上文"librga 从哪来"。

## 四、播放链路需要的其他 GStreamer 插件（通常由板卡镜像自带）

| 插件 | 提供方 | 用途 |
| --- | --- | --- |
| mppvideodec / mpph264enc 等 | Rockchip gstreamer-rockchip（`libgstrockchipmpp.so`） | H.264/H.265/JPEG 硬解 |
| kmssink | Rockchip 定制 gst-plugins-bad | 直接输出 DRM/KMS |
| qtdemux / h264parse | gstreamer1.0-plugins-good / bad | 解封装、码流解析 |

检测：

```bash
gst-inspect-1.0 mppvideodec kmssink rgaconv
```

如缺失（常见于非 Rockchip 官方镜像），Debian 下可补：

```bash
sudo apt install -y gstreamer1.0-tools gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad
```

## 五、安装与验证

```bash
# 安装插件
sudo dpkg -i packages/gstreamer1.0-rgaconv_1.0-1_arm64.deb
# 依赖缺失时自动补齐
sudo apt-get -f install

# 验证插件被 GStreamer 识别
gst-inspect-1.0 rgaconv
# 输出应包含：rgaconv: Rockchip RGA color converter

# 验证 librga 与 RGA 设备
ls -l /dev/rga
```

## 六、常见问题

1. `gst-inspect-1.0` 里找不到 rgaconv：确认 `.so` 在 `/usr/lib/aarch64-linux-gnu/gstreamer-1.0/`，且 `librga.so` 可被加载（`ldd /usr/lib/aarch64-linux-gnu/gstreamer-1.0/libgstrgaconv.so`）。
2. 加载时报 `liblibrga.so` 缺失：先装 `librga2`。
3. 协商失败（管道卡在 PAUSED/报 not-negotiated）：确认上下游格式在插件 caps 范围内（见 README"修复与关键实现点"第 1 条）。
4. 画面花屏/颜色错乱：确认使用的是本仓库修正 stride 后的版本（`GST_VIDEO_INFO_PLANE_STRIDE/OFFSET`）。
5. 老系统（如 Debian 10，glibc 较旧）：建议直接在目标板源码编译，不要用本 deb。