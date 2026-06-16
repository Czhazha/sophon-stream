# websocket_push

WebSocket 目标识别结果推送元素。将检测/跟踪结果以 JSON 格式通过 WebSocket 发送到服务端。

## 功能

- 将目标检测与跟踪的**元数据**（帧信息、目标框、类别、置信度、跟踪 ID）序列化为 JSON
- 通过 WebSocket 客户端连接到远端服务端并发送消息
- **不包含图像 base64 数据**，消息体积紧凑，适合网络传输
- 自动重连机制，连接断开后每 3 秒尝试重连
- 按通道维护独立的 WebSocket 连接
- 内置消息队列，防止推流阻塞

## JSON 消息格式

```json
{
  "mFrame": {
    "mChannelId": 2,
    "mFrameId": 1234,
    "mTimestamp": 1702345678000,
    "mWidth": 1920,
    "mHeight": 1080
  },
  "mChannelIdInternal": 2,
  "objects": [
    {
      "mBox": { "mX": 100, "mY": 200, "mWidth": 300, "mHeight": 400 },
      "mScores": [0.95, 0.03, 0.02],
      "mClassify": 0,
      "mTrackId": 5
    }
  ],
  "mSubId": 0,
  "mGraphId": 0
}
```

## 配置参数

| 参数 | 类型 | 说明 |
|------|------|------|
| ip | string | WebSocket 服务端 IP 地址 |
| port | int | WebSocket 服务端端口 |
| path | string | WebSocket 路径（如 "/ws/detection"） |

## 配置示例

```json
{
    "configure": {
        "ip": "192.168.150.2",
        "port": 8765,
        "path": "/ws/detection"
    },
    "shared_object": "../../build/lib/libwebsocket_push.so",
    "name": "websocket_push",
    "side": "sophgo",
    "thread_number": 4,
    "is_sink": true
}
```

## Pipeline 集成

推荐在 `bytetrack` 之后插入 `distributor`，将数据分发为两路：
- 一路继续到 `customosd` → `qt_display`（显示）
- 一路到 `websocket_push`（消息推送）

```
bytetrack → distributor → customosd → qt_display
                        → websocket_push (sink)
```
