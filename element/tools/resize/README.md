# sophon-stream resize element


sophon-stream resize element是sophon-stream框架中的一个插件，是一个用于尺寸变换的插件。

## 1. 特性
目前插件支持从原图裁剪指定区域的图像，并缩放到指定大小。

可选开启原图 CPU 采样缓存，供下游 customosd 过线抓拍使用。

## 2. 配置参数
sophon-stream resize插件具有一些可配置的参数，可以根据需求进行设置。以下是一些常用的参数：

```json
{
  "configure": {
    "dst_h": 512,
    "dst_w": 1024,
    "crop_w": 4096,
    "crop_h": 2048,
    "crop_top": 0,
    "crop_left": 0,
    "origin_cache_interval": 5,
    "origin_cache_depth": 3,
    "origin_cache_max_gap": 10
  },
  "shared_object": "../../build/lib/libresize.so",
  "name": "resize",
  "side": "sophgo",
  "thread_number": 1
}

```

| 参数名        | 类型   | 默认值                         | 说明             |
| ------------- | ------ | ------------------------------| ---------------- |
| dst_h         | int    | 无                            | 输出图像的高度信息 |
| dst_w         | int    | 无                            | 输出图像的宽度信息 |
| crop_top      | int    | 无                            | 对输入图像进行裁剪操作时，从哪一行开始进行裁剪的位置信息 |
| crop_left     | int    | 无                            | 对输入图像进行裁剪操作时，从哪一列开始进行裁剪的位置信息 |
| crop_h        | int    | 无                            | 对输入图像进行裁剪操作时，裁剪出图像的高度信息 |
| crop_w        | int    | 无                            | 对输入图像进行裁剪操作时，裁剪出图像的宽度信息 |
| origin_cache_interval | int | 0                      | 原图 CPU 缓存采样间隔；`0` 表示关闭；`N` 表示每 N 帧采样 1 帧做 `toMAT` 并缓存到 CPU |
| origin_cache_depth | int | 3                         | 每路通道保留最近多少个采样原图 |
| origin_cache_max_gap | int | `interval * 2`            | customosd 压线保存时，允许使用的缓存帧与当前检测帧的最大 `frame_id` 差值 |
| shared_object | string | "../../../build/lib/libresize.so" | libresize动态库路径 |
| name          | string | "resize"                       | element名称      |
| side          | string | "sophgo"                       | 设备类型         |
| thread_number | int    | 1                              | 启动线程数       |

### 原图缓存说明

* 开启后仅在采样帧执行一次 D2H（`toMAT`），设备原图在缩放后照常释放，不增加设备内存占用。
* 缓存按 `channel_id` 隔离，供 customosd 在压线时按 `frame_id` 查找最近可用原图。
* 保存的是「压线时刻附近最近一次采样」的原图，不一定是压线精确帧。
