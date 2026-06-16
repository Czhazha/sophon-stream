# mqtt_push

MQTT 目标识别结果推送元素。将检测/跟踪结果以 JSON 格式通过 MQTT 发布到 Broker。

## 功能

- 将目标检测与跟踪的**元数据**（帧信息、目标框、类别、置信度、跟踪 ID）序列化为 JSON
- 通过 MQTT 协议发布到指定 Broker 的 Topic
- **不包含图像 base64 数据**，消息体紧凑
- mosquittopp 内置自动重连机制
- 按通道维护独立的 MQTT 连接
- 内置消息队列，防止推流阻塞

## 依赖

- `libmosquitto`
- `libmosquittopp`（C++ wrapper）

板上安装：
```bash
sudo apt install libmosquittopp-dev
```

交叉编译时设置 `MOSQUITTO_PATH` 指向 aarch64 的头文件和库目录。

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
| broker_ip | string | MQTT Broker IP 地址 |
| broker_port | int | MQTT Broker 端口（默认 1883） |
| topic | string | 发布消息的 Topic |
| client_id | string | 客户端 ID（可选，留空自动生成） |

## 配置示例

```json
{
    "configure": {
        "broker_ip": "192.168.150.2",
        "broker_port": 1883,
        "topic": "algo",
        "client_id": "sophon_stream"
    },
    "shared_object": "../../build/lib/libmqtt_push.so",
    "name": "mqtt_push",
    "side": "sophgo",
    "thread_number": 4,
    "is_sink": true
}
```

## Pipeline 集成

```json
{
  "connections": [
    {"src_id": 5004, "src_port": 0, "dst_id": 5005, "dst_port": 0},
    {"src_id": 5004, "src_port": 0, "dst_id": 5008, "dst_port": 0}
  ]
}
```

通过 distributor 分发：
```
bytetrack → distributor → customosd → qt_display
                        → mqtt_push (sink)
```
