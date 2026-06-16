//===----------------------------------------------------------------------===//
//
// Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
// SOPHON-STREAM is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//

#ifndef SOPHON_STREAM_ELEMENT_MQTT_PUSH_H_
#define SOPHON_STREAM_ELEMENT_MQTT_PUSH_H_

#include <mosquitto.h>

#include <mutex>
#include <nlohmann/json.hpp>
#include <queue>
#include <string>

#include "common/object_metadata.h"
#include "common/profiler.h"
#include "element.h"

namespace sophon_stream {
namespace element {
namespace mqtt_push {

/**
 * @brief MQTT 推送实现类，使用 mosquitto C API（非 deprecated C++ wrapper）
 *        每个视频通道一个实例
 */
class MqttPushImpl_ {
 public:
  MqttPushImpl_(std::string& brokerIp, int brokerPort, std::string& topic,
                std::string& clientId, int channel);
  ~MqttPushImpl_();

  bool pushQueue(std::shared_ptr<nlohmann::json> j);
  void release();

  bool isConnected() const { return mConnected; }

 private:
  static void onConnectCb(struct mosquitto* mosq, void* userdata, int rc);
  static void onDisconnectCb(struct mosquitto* mosq, void* userdata, int rc);

  std::queue<std::shared_ptr<nlohmann::json>> objQueue;
  std::thread workThread;
  void postFunc();

  std::shared_ptr<nlohmann::json> popQueue();
  size_t getQueueSize();
  std::mutex mtx;
  constexpr static int maxQueueLen = 20;

  bool isRunning_ = true;
  struct mosquitto* mMosq = nullptr;
  std::string mTopic;
  std::string mBrokerIp;
  int mBrokerPort;
  bool mConnected = false;

  std::string mFpsProfilerName;
  ::sophon_stream::common::FpsProfiler mFpsProfiler;
};

class MqttPush : public ::sophon_stream::framework::Element {
 public:
  MqttPush();
  ~MqttPush() override;

  common::ErrorCode initInternal(const std::string& json) override;

  common::ErrorCode doWork(int dataPipeId) override;

  static constexpr const char* CONFIG_INTERNAL_BROKER_IP_FIELD = "broker_ip";
  static constexpr const char* CONFIG_INTERNAL_BROKER_PORT_FIELD =
      "broker_port";
  static constexpr const char* CONFIG_INTERNAL_TOPIC_FIELD = "topic";
  static constexpr const char* CONFIG_INTERNAL_CLIENT_ID_FIELD = "client_id";

 private:
  std::unordered_map<int, std::shared_ptr<MqttPushImpl_>> mapImpl_;
  std::mutex mapMtx;
  std::string brokerIp_;
  int brokerPort_;
  std::string topic_;
  std::string clientId_;
};

}  // namespace mqtt_push
}  // namespace element
}  // namespace sophon_stream

#endif
