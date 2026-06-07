# YOLOv5 Detection-Track-QT-Display Demo

English | [简体中文](README.md)

## 1. Introduction

This sample builds a **YOLOv5** detection + **ByteTrack** tracking pipeline with **OSD** and **Qt** multi-view HDMI display using sophon-stream.

Pipeline: `decode → yolov5_group → bytetrack → osd → qt_display`

## 2. Features

* YOLOv5 for detection (`yolov5_group` plugin);
* ByteTrack for tracking;
* BM1684X / BM1684 / BM1688 (SoC);
* Multi-stream input and multi-threading;
* Qt HDMI display (2×3 grid).

## 3. Prepare Models and Data

```bash
sudo apt install unzip
chmod -R +x scripts/
./scripts/download.sh
```

Default model (BM1684X + tpu_kernel):

```text
data/models/BM1684X_tpukernel/yolov5s_tpukernel_int8_1b.bmodel
```

Adjust `model_path` in [yolov5_group.json](./config/yolov5_group.json) for other platforms.

## 4. Environment

See [Environment Install Guide](../../docs/EnvironmentInstallGuide_EN.md) and install `qtbase5-dev` on PCIe hosts. For SoC cross-compile, refer to [yolox_bytetrack_osd_qt](../yolox_bytetrack_osd_qt/README_EN.md).

## 5. Build

See [HowToMake](../../docs/HowToMake_EN.md). Required plugins: `yolov5`, `bytetrack`, `decode`, `osd`, `qt_display`.

## 6. Run

**PCIe:**

```bash
cd samples/build
./main --demo_config_path=../yolov5_bytetrack_osd_qt/config/yolov5_bytetrack_osd_qt_demo.json
```

**SoC HDMI:**

```bash
sudo systemctl stop SophonHDMI.service
cd samples/yolov5_bytetrack_osd_qt/scripts
sudo ./run_hdmi_show.sh
```

Config files are under [./config](./config). Reduce `channels` if NPU memory is insufficient.

## 7. Performance

OSD drawing limits end-to-end FPS; see [yolov5](../yolov5/README_EN.md) for inference-only benchmarks.
