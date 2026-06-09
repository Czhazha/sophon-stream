# YOLOv5 目标跟踪算法结果显示 Demo

## 目录
- [1. 简介](#1-简介)
- [2. 特性](#2-特性)
- [3. 准备模型与数据](#3-准备模型与数据)
- [4. 环境准备](#4-环境准备)
- [5. 程序编译](#5-程序编译)
- [6. 程序运行](#6-程序运行)
- [7. 性能测试](#7-性能测试)

## 1. 简介

本例程用于说明如何使用 sophon-stream 快速构建基于 **YOLOv5** 的视频目标检测与 **ByteTrack** 跟踪应用，并通过 **customosd + Qt** 将多路算法结果显示到 HDMI。

流水线：`decode → resize → yolov5_group → bytetrack → customosd → qt_display`

## 2. 特性

* 检测模型使用 YOLOv5（yolov5_group 插件）；
* 跟踪模型使用 ByteTrack；
* 使用 customosd 支持按通道 ROI 过滤、拌线绘制与过线抓拍；
* 支持 BM1684X（x86 PCIe、SoC）、BM1684（x86 PCIe、SoC、arm PCIe）、BM1688（SoC）；
* 支持多路视频流、多线程；
* 支持 Qt 多画面 HDMI 显示。

## 3. 准备模型与数据

在 `scripts` 目录下执行 [download.sh](./scripts/download.sh)：

```bash
sudo apt install unzip
chmod -R +x scripts/
./scripts/download.sh
```

脚本会在 `data/` 下生成 `models/` 与 `videos/`。默认检测模型路径（BM1684X + tpu_kernel）：

```text
data/models/BM1684X_tpukernel/yolov5s_tpukernel_int8_1b.bmodel
```

其他平台请修改 [yolov5_group.json](./config/yolov5_group.json) 中的 `model_path`，例如 BM1684X 非 tpu_kernel 可使用 `data/models/BM1684X/` 下对应 bmodel。

测试视频为 MOT17 片段（与 yolox_bytetrack_osd_qt 相同），便于验证跟踪效果。

## 4. 环境准备

### 4.1 x86/arm PCIe 平台

安装 libsophon、sophon-opencv、sophon-ffmpeg，参见 [环境搭建](../../docs/EnvironmentInstallGuide.md)。另需公版 Qt：

```bash
sudo apt install qtbase5-dev
```

### 4.2 SoC 平台

BM1684/BM1684X 交叉编译需 sophon-qt；BM1688 需 arm 公版 qt，参见 [yolox_bytetrack_osd_qt](../yolox_bytetrack_osd_qt/README.md) 第 4.2 节。

## 5. 程序编译

参见 [sophon-stream 编译](../../docs/HowToMake.md)。需编译 element：`yolov5`、`bytetrack`、`decode`、`customosd`、`resize`、`qt_display`。

## 6. 程序运行

### 6.1 Json 配置说明

配置文件位于 [./config](./config)：

| 文件 | 说明 |
|------|------|
| [yolov5_bytetrack_osd_qt_demo.json](./config/yolov5_bytetrack_osd_qt_demo.json) | 多路输入通道（`channel_id` 需与 customosd 中一致） |
| [engine_group.json](./config/engine_group.json) | Graph 与 element 连接 |
| [yolov5_group.json](./config/yolov5_group.json) | YOLOv5 检测 |
| [bytetrack.json](./config/bytetrack.json) | 跟踪 |
| [customosd.json](./config/customosd.json) | ROI 过滤、拌线绘制、过线抓拍 |
| [resize.json](./config/resize.json) | 缩放；`origin_cache_interval` 开启原图 CPU 采样缓存供过线抓拍 |
| [qt_display.json](./config/qt_display.json) | Qt 拼接显示（2 行 × 3 列） |

**customosd 要点**：

* `channels[].channel_id` 与 demo 中每路 `channel_id` 对应（本例为 2、3、20、30）；
* `rois`：多边形 ROI，检测框中心不在 ROI 内则不显示；
* `lines`：拌线端点，用于过线判断；
* `save_path`：过线图片保存目录；**留空或不配置则不保存**；
* `save_image_mode`：仅当 `save_path` 非空时生效。`clean` 保存干净原图，`annotated` 保存带 ROI/拌线/跟踪框的原图；
* 过线保存原图需在 [resize.json](./config/resize.json) 中设置 `origin_cache_interval`（如 `5` 表示 5 帧采 1 帧缓存到 CPU）。
* customosd 每路每 100 帧输出一次 `decode_to_output` 平均延迟（毫秒），用于量化 pipeline 延迟。

详细参数见 [customosd README](../../element/tools/customosd/README.md)。

内存不足时可减少 `channels` 中的路数。

### 6.2 运行

**PCIe：**

```bash
cd samples/build
sudo ./main --demo_config_path=../yolov5_bytetrack_osd_qt/config/yolov5_bytetrack_osd_qt_demo.json
```

**SoC HDMI：**

```bash
sudo systemctl stop SophonHDMI.service
cd samples/yolov5_bytetrack_osd_qt/scripts
sudo ./run_hdmi_show.sh
```

## 7. 性能测试

OSD 绘图较慢，本例程暂不提供性能测试结果；YOLOv5 纯推理性能请参考 [yolov5](../yolov5/README.md) 例程。
