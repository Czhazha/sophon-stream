# sophon-stream customosd element

sophon-stream customosd element 是 sophon-stream 框架中的自定义可视化插件，在标准 osd 基础上扩展了按通道 ROI 过滤、拌线检测、过线抓拍保存等功能。需配合 **bytetrack** 使用。

## 1. 特点

* 按 `channel_id` 配置多个 ROI 多边形区域
* 按 `channel_id` 配置多条拌线（每线 2 个端点）
* ROI 过滤：检测框中心点不在任一 ROI 内则丢弃且不绘制
* 拌线检测：根据跟踪 ID 判断目标轨迹是否穿过拌线
* 同一目标对同一条线仅保存首张过线图片（需配置 `save_path`）
* 输出每帧解码到 customosd 的端到端延迟日志（毫秒）
* 在画面上绘制 ROI（绿色）与拌线（红色）及跟踪框

## 2. 配置参数

```json
{
  "configure": {
    "class_names_file": "../data/coco.names",
    "put_text": true,
    "drop_interval": 1,
    "save_path": "./customosd_results",
    "save_image_mode": "clean",
    "channels": [
      {
        "channel_id": 0,
        "rois": [
          [
            { "left": 200, "top": 150 },
            { "left": 900, "top": 150 },
            { "left": 900, "top": 700 },
            { "left": 200, "top": 700 }
          ]
        ],
        "lines": [
          [
            { "left": 550, "top": 150 },
            { "left": 550, "top": 700 }
          ]
        ]
      }
    ]
  },
  "shared_object": "../../../build/lib/libcustomosd.so",
  "name": "customosd",
  "side": "sophgo",
  "thread_number": 4
}
```

| 参数名 | 类型 | 默认值 | 说明 |
| :----- | :--- | :----- | :--- |
| class_names_file | 字符串 | 无 | 类别名称文件路径，`put_text` 为 true 时用于显示类别 |
| put_text | 布尔值 | false | 是否在检测框旁绘制 track_id 与类别名 |
| drop_interval | 整数 | 1 | 丢帧：每 N 帧只保留第 N 帧，其余帧不处理且**不向下游传递**；`0` 或 `1` 表示不丢帧 |
| save_path | 字符串 | 空 | 过线抓拍图片保存目录；**为空或不配置则不保存** |
| save_image_mode | 字符串 | clean | 过线抓拍图片内容；**仅当 `save_path` 非空时生效**。`clean`：无标注画面；`annotated`：叠加 ROI、拌线与跟踪框后的画面 |
| channels | 数组 | 无 | 各通道 ROI / 拌线规则，见下表 |
| shared_object | 字符串 | 无 | libcustomosd 动态库路径 |
| name | 字符串 | "customosd" | element 名称 |
| side | 字符串 | "sophgo" | 设备类型 |
| thread_number | 整数 | 1 | 线程数，建议与输入路数一致 |

### channels 子项

| 参数名 | 类型 | 说明 |
| :----- | :--- | :--- |
| channel_id | 整数 | 与 demo json 中 `channels[].channel_id` 一致 |
| rois | 数组 | 多个 ROI，每个 ROI 为至少 3 个顶点的多边形；`left` 为 X，`top` 为 Y；为空表示不做 ROI 过滤 |
| lines | 数组 | 多条拌线，每条为 2 个端点；为空表示不做拌线检测 |

## 3. 行为说明

1. **ROI 过滤**：以检测框中心点判断；点在任一 ROI 多边形内则保留（OR 关系），否则从结果中移除且不画框。
2. **拌线判断**：比较同一 `track_id` 相邻两帧中心点轨迹与拌线线段是否相交。
3. **过线保存**：仅当 `save_path` 非空时，对 `(channel_id, track_id, line_index)` 首次过线保存一张 JPG，文件名格式：`channel_{id}_frame_{frameId}_{timestamp}.jpg`。
   * `clean` 模式保存绘制前的当前帧；`annotated` 模式保存叠加 ROI、拌线与跟踪框后的画面。
4. **绘制**：ROI 绿色闭合多边形；拌线红色线段；保留的跟踪目标绘制彩色框。
5. **丢帧**：受 `drop_interval` 控制，`N>1` 时每 N 帧只保留第 N 帧（如 `3` 表示每 3 帧留 1 帧），其余帧本 element 直接返回、不转发下游。
6. **耗时日志**：每路每 100 帧输出一次 `CustomOsd doWork start/end: channel=..., time=HH:MM:SS.mmm`。
7. **延迟日志**：每路每 100 帧输出一次平均延迟 `CustomOsd output latency avg: channel=..., frames=100, decode_to_output=...ms`，表示 `mFrame->mTimestamp`（解码时刻）到 customosd 输出时刻差值的滑动平均。

## 4. 流水线要求

```
decode → resize → yolov5/yolox → bytetrack → customosd → encode/qt_display/...
```

* 上游必须包含 **bytetrack**，否则无法获取 `track_id`，拌线与过线保存不会生效。
* ROI / 拌线坐标基于 resize 后的画面尺寸配置（与检测坐标系一致）。

## 5. 示例

参考 [yolov5_bytetrack_osd_qt](../../../samples/yolov5_bytetrack_osd_qt/config/customosd.json) 与 [resize.json](../../../samples/yolov5_bytetrack_osd_qt/config/resize.json)。
