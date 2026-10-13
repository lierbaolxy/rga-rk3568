# rga-rk3568

RK3568/RK3588 平台 **Rockchip RGA 硬件色彩转换 GStreamer 插件（rgaconv）** 及配套工具。

## 解决的问题

RK3568 + HDMI + `kmssink` 播放 1080p H.264 时：`mppvideodec` 输出 NV12，而 `kmssink` 需要 RGB（BGRx/BGRA/RGBx/RGBA），中间用软件转换（`videoconvert`）会把 CPU 占满（实测 **97%**）。

本仓库用 RGA（librga im2d）把 YUV→RGB 转换下沉到硬件，实测 1080p **CPU 占用 97% → 17%**。

## 目录结构

```
rga-rk3568/
├── README.md                # 本文件：说明、修复点、构建、安装、验证
├── DEPENDENCIES.md          # 依赖清单与安装步骤
├── src/
│   ├── gstrgaconv.c         # rgaconv 插件源码（唯一必需文件）
│   ├── kmswake.c            # KMS 唤醒小工具（kmssink 起播辅助）
│   └── README.md            # 原始编译说明（含关键实现要点）
└── packages/
    └── gstreamer1.0-rgaconv_1.0-1_arm64.deb   # 预编译安装包（arm64）
```

## 修复与关键实现点

1. **caps 协商修复（transform_caps）**
   先把输入侧 caps 的 `format` 字段移除，再与对端 pad 模板求交集。
   若直接拿输入 caps（如 NV12）与输出模板（BGRx）相交会协商出 `EMPTY`，管道无法启动。
2. **MPP 16 行对齐导致的"花屏"修复**
   Rockchip MPP 输出按 16 行对齐（1080 → 1088），按紧密排列推算 chroma 平面偏移会把色度平面放错位置，表现为花屏/颜色错乱。
   改为用 `GST_VIDEO_INFO_PLANE_STRIDE` / `GST_VIDEO_INFO_PLANE_OFFSET` 取**真实** stride/offset 后再交给 RGA。
3. **转换实现**
   调用 `imcvtcolor()`（librga im2d 同步 blit），1080p 每帧约 2~4 ms；不引入额外线程/队列。
4. **支持格式**
   输入：NV12 / NV16 / I420 / NV21（sysmem）
   输出：BGRx / BGRA / RGBx / RGBA（sysmem），要求同尺寸。
5. **kmswake（配套工具）**
   `kmssink` 要求 encoder→crtc 链路已激活才能起播；`kmswake` 一次性激活指定 connector 的首选模式（黑图，进程退出后 fb 释放、屏幕保持黑），解决"kmssink 无法启动"的问题。

## 快速使用

### 1) 安装依赖

见 [DEPENDENCIES.md](DEPENDENCIES.md)。

### 2) 安装预编译 deb（推荐）

```bash
sudo dpkg -i packages/gstreamer1.0-rgaconv_1.0-1_arm64.deb
gst-inspect-1.0 rgaconv     # 应输出 Rockchip RGA color converter
```

### 3) 或从源码编译

```bash
cd src
gcc -shared -fPIC -O2 -DPACKAGE='"rgaconv"' -DVERSION='"1.0"' \
  -o libgstrgaconv.so gstrgaconv.c \
  $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-base-1.0 \
    gstreamer-video-1.0 librga)
sudo cp libgstrgaconv.so /usr/lib/aarch64-linux-gnu/gstreamer-1.0/
```

### 4) 播放验证（RK3568 + HDMI，connector/plane id 按实际调整）

```bash
gst-launch-1.0 filesrc location=flower1920x1080.mp4 ! qtdemux ! h264parse ! \
  mppvideodec ! rgaconv ! queue ! \
  kmssink connector-id=172 plane-id=57 \
  render-rectangle="<0,0,2560,1440>" force-aspect-ratio=false
```

### kmswake 用法（可选）

```bash
gcc -O2 -o kmswake kmswake.c $(pkg-config --cflags --libs libdrm)
sudo ./kmswake /dev/dri/card0 172 85 57   # [card] [conn_id] [crtc_id] [plane_id]
```

## 已验证环境

| 项目 | 值 |
| --- | --- |
| 板卡 | RK3568（3568A），aarch64 |
| 系统 | Debian GNU/Linux 11 (bullseye)，内核 5.10.209 |
| GStreamer | 1.18.5 |
| librga | 2.2.0（librga2 / librga-dev，hold 状态） |
| 显示 | HDMI + kmssink（connector-id=172，plane-id=57） |

## 验证结果

- 1080p 播放：CPU 占用 **97% → 17%**（软件转换 → RGA 硬件转换）
- 画面：色度平面步距修正后无花屏、颜色正常

## 说明

- deb 包与 `src/` 源码同版本（插件版本 1.0），仅含 `libgstrgaconv.so`。
- `kmswake` 为独立小工具，不随 deb 安装，按需自行编译。
- 本仓库内容整理自 RK3568 设备 `/userdata/lxy/rga-tools/`。