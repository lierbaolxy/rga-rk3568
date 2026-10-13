# rgaconv 插件编译说明

Rockchip RGA 硬件色彩转换 GStreamer 元件（NV12/NV16/I420/NV21 → BGRx/BGRA/RGBx/RGBA），适用于 RK356x / RK3588。

## 源码位置

| 文件 | 说明 |
| --- | --- |
| [gstrgaconv.c](file:///home/shimeta/rga-tools/gstrgaconv.c) | rgaconv 元件源码（唯一必需文件） |
| [kmswake.c](file:///home/shimeta/rga-tools/kmswake.c) | KMS 唤醒小工具（与本插件无关） |

同一份源码的编译产物已打入 deb 包：`/home/shimeta/gstreamer1.0-rgaconv_1.0-1_arm64.deb`。

## 编译依赖

```bash
sudo apt install gcc libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev librga-dev
```

本机已验证版本：gstreamer 1.18.5，librga 2.2.0（arm64）。

## 编译命令

一条命令即可（源码头部注释里也内置了同样命令）：

```bash
cd /home/shimeta/rga-tools
gcc -shared -fPIC -O2 -DPACKAGE='"rgaconv"' -DVERSION='"1.0"' \
  -o libgstrgaconv.so gstrgaconv.c \
  $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-base-1.0 \
    gstreamer-video-1.0 librga)
```

## 安装

```bash
sudo cp libgstrgaconv.so /usr/lib/aarch64-linux-gnu/gstreamer-1.0/
```

## 验证

```bash
gst-inspect-1.0 rgaconv
```

能看到 "Rockchip RGA color converter" 即成功。

播放测试（RK3568 + HDMI，已验证 CPU 占用从 97% 降至 17%）：

```bash
gst-launch-1.0 filesrc location=/home/shimeta/flower1920x1080.mp4 ! qtdemux ! \
  h264parse ! mppvideodec ! rgaconv ! queue ! \
  kmssink connector-id=172 plane-id=57 \
  render-rectangle="<0,0,2560,1440>" force-aspect-ratio=false
```

## 关键实现要点

- 用 `GST_VIDEO_INFO_PLANE_STRIDE/OFFSET` 取真实 stride，MPP 输出按 16 行对齐（1080 → 1088），按紧密排列推算 chroma 平面会导致花屏。
- 转换调用 `imcvtcolor()`（librga im2d 同步 blit），1080p 每帧约 2~4 ms。
- `transform_caps` 中先移除输入侧 format 再与对端模板求交集，否则 NV12 vs BGRx 协商会得 EMPTY。

## 重新打包 deb

```bash
mkdir -p pkg/DEBIAN pkg/usr/lib/aarch64-linux-gnu/gstreamer-1.0
cp libgstrgaconv.so pkg/usr/lib/aarch64-linux-gnu/gstreamer-1.0/
# control 文件参考 /tmp/rgaconv-deb/gstreamer1.0-rgaconv/DEBIAN/control
dpkg-deb --build pkg gstreamer1.0-rgaconv_1.0-1_arm64.deb
```
