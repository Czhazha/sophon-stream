//===----------------------------------------------------------------------===//
//
// Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
// SOPHON-STREAM is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//

#include "mqtt_push.h"

#include "common/common_defs.h"
#include "common/logger.h"
#include "common/serialize.h"
#include "element_factory.h"

namespace sophon_stream {
namespace element {
namespace mqtt_push {

// -------- 序列化（不含图像 base64，与 websocket_push 一致） --------

static nlohmann::json serializeMetadata(
    const std::shared_ptr<common::ObjectMetadata>& obj) {
  nlohmann::json j;

  if (obj->mFrame) {
    j["mFrame"]["mChannelId"] = obj->mFrame->mChannelId;
    j["mFrame"]["mFrameId"] = obj->mFrame->mFrameId;
    j["mFrame"]["mTimestamp"] = obj->mFrame->mTimestamp;
    j["mFrame"]["mWidth"] = obj->mFrame->mWidth;
    j["mFrame"]["mHeight"] = obj->mFrame->mHeight;
    j["mChannelIdInternal"] = obj->mFrame->mChannelIdInternal;
  }

  size_t detCount = obj->mDetectedObjectMetadatas.size();
  size_t trackCount = obj->mTrackedObjectMetadatas.size();
  size_t objCount = std::max(detCount, trackCount);
  for (size_t i = 0; i < objCount; i++) {
    nlohmann::json objJ;
    if (i < detCount) {
      auto& det = obj->mDetectedObjectMetadatas[i];
      objJ["mBox"]["mX"] = det->mBox.mX;
      objJ["mBox"]["mY"] = det->mBox.mY;
      objJ["mBox"]["mWidth"] = det->mBox.mWidth;
      objJ["mBox"]["mHeight"] = det->mBox.mHeight;
      objJ["mScores"] = det->mScores;
      objJ["mClassify"] = det->mClassify;
    }
    if (i < trackCount) {
      objJ["mTrackId"] = obj->mTrackedObjectMetadatas[i]->mTrackId;
    } else {
      objJ["mTrackId"] = -1;
    }
    j["objects"].push_back(objJ);
  }

  j["mSubId"] = obj->mSubId;
  j["mGraphId"] = obj->mGraphId;

  return j;
}

// ==================== MqttPushImpl_ ====================

MqttPushImpl_::MqttPushImpl_(std::string& brokerIp, int brokerPort,
                             std::string& topic, std::string& clientId,
                             int channel)
    : mBrokerIp(brokerIp),
      mBrokerPort(brokerPort),
      mTopic(topic) {
  // 创建 mosquitto 客户端实例
  const char* id = clientId.empty() ? nullptr : clientId.c_str();
  mMosq = mosquitto_new(id, true /* clean_session */, this);
  if (!mMosq) {
    IVS_WARN("MQTT: mosquitto_new failed for broker {0}:{1}", mBrokerIp,
             mBrokerPort);
    return;
  }

  // 注册回调
  mosquitto_connect_callback_set(mMosq, onConnectCb);
  mosquitto_disconnect_callback_set(mMosq, onDisconnectCb);

  // 配置自动重连：初始 1s，最大 30s，指数退避
  mosquitto_reconnect_delay_set(mMosq, 1, 30, true);

  // 连接 broker
  int rc = mosquitto_connect(mMosq, mBrokerIp.c_str(), mBrokerPort,
                             60 /* keepalive */);
  if (rc != MOSQ_ERR_SUCCESS) {
    IVS_WARN("MQTT connect() returned {0} for broker {1}:{2}", rc, mBrokerIp,
             mBrokerPort);
  }

  // 启动 mosquitto 内部网络线程
  mosquitto_loop_start(mMosq);

  workThread = std::thread(&MqttPushImpl_::postFunc, this);

  mFpsProfilerName = "mqtt_push_ch" + std::to_string(channel) + "_fps";
  mFpsProfiler.config(mFpsProfilerName, 100);
}

MqttPushImpl_::~MqttPushImpl_() { release(); }

void MqttPushImpl_::release() {
  isRunning_ = false;
  if (mMosq) {
    mosquitto_disconnect(mMosq);
    mosquitto_loop_stop(mMosq, true /* force */);
    mosquitto_destroy(mMosq);
    mMosq = nullptr;
  }
  if (workThread.joinable()) workThread.join();
}

// ---- mosquitto 回调（静态函数） ----

void MqttPushImpl_::onConnectCb(struct mosquitto* mosq, void* userdata,
                                int rc) {
  auto* self = static_cast<MqttPushImpl_*>(userdata);
  if (rc == MOSQ_ERR_SUCCESS) {
    self->mConnected = true;
    IVS_INFO("MQTT connected to {0}:{1}", self->mBrokerIp, self->mBrokerPort);
  } else {
    self->mConnected = false;
    IVS_WARN("MQTT connection failed to {0}:{1}, rc={2}", self->mBrokerIp,
             self->mBrokerPort, rc);
  }
}

void MqttPushImpl_::onDisconnectCb(struct mosquitto* mosq, void* userdata,
                                   int rc) {
  auto* self = static_cast<MqttPushImpl_*>(userdata);
  self->mConnected = false;
  if (rc == 0) {
    IVS_INFO("MQTT disconnected cleanly from {0}:{1}", self->mBrokerIp,
             self->mBrokerPort);
  } else {
    IVS_WARN("MQTT disconnected unexpectedly from {0}:{1}, rc={2}",
             self->mBrokerIp, self->mBrokerPort, rc);
  }
  // mosquitto 会在 loop 中自动重连
}

// ---- 消息发送线程 ----

void MqttPushImpl_::postFunc() {
  while (isRunning_) {
    auto ptr = popQueue();
    if (ptr == nullptr) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    if (!mConnected) {
      // 未连接则稍等重试
      std::lock_guard<std::mutex> lock(mtx);
      if (objQueue.size() < maxQueueLen) {
        objQueue.push(ptr);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    mFpsProfiler.add(1);
    std::string payload = ptr->dump();
    int rc = mosquitto_publish(mMosq, nullptr, mTopic.c_str(),
                               payload.size(), payload.c_str(), 1 /* qos */,
                               false /* retain */);
    if (rc != MOSQ_ERR_SUCCESS) {
      IVS_WARN("MQTT publish error rc={0}, topic={1}", rc, mTopic);
      mConnected = false;
      // 重新入队
      std::lock_guard<std::mutex> lock(mtx);
      if (objQueue.size() < maxQueueLen) {
        objQueue.push(ptr);
      }
    }
  }
}

// ---- 队列操作 ----

bool MqttPushImpl_::pushQueue(std::shared_ptr<nlohmann::json> j) {
  size_t len = getQueueSize();
  if (len >= maxQueueLen) return false;
  {
    std::lock_guard<std::mutex> lock(mtx);
    objQueue.push(j);
  }
  return true;
}

std::shared_ptr<nlohmann::json> MqttPushImpl_::popQueue() {
  std::lock_guard<std::mutex> lock(mtx);
  std::shared_ptr<nlohmann::json> j = nullptr;
  if (objQueue.empty()) return j;
  j = objQueue.front();
  objQueue.pop();
  return j;
}

size_t MqttPushImpl_::getQueueSize() {
  size_t len = 0;
  {
    std::lock_guard<std::mutex> lock(mtx);
    len = objQueue.size();
  }
  return len;
}

// ==================== MqttPush ====================

MqttPush::MqttPush() {
  // 全局初始化 mosquitto 库（仅一次）
  static std::once_flag initFlag;
  std::call_once(initFlag, []() { mosquitto_lib_init(); });
}

MqttPush::~MqttPush() {
  for (auto [k, v] : mapImpl_) {
    v->release();
  }
}

common::ErrorCode MqttPush::initInternal(const std::string& json) {
  common::ErrorCode errorCode = common::ErrorCode::SUCCESS;
  do {
    auto configure = nlohmann::json::parse(json, nullptr, false);
    if (!configure.is_object()) {
      errorCode = common::ErrorCode::PARSE_CONFIGURE_FAIL;
      break;
    }
    auto ipIt = configure.find(CONFIG_INTERNAL_BROKER_IP_FIELD);
    STREAM_CHECK(
        (ipIt != configure.end() && ipIt->is_string()),
        "broker_ip must be string, please check your mqtt_push element "
        "configuration file");
    brokerIp_ = ipIt->get<std::string>();
    auto portIt = configure.find(CONFIG_INTERNAL_BROKER_PORT_FIELD);
    STREAM_CHECK(
        (portIt != configure.end() && portIt->is_number_integer()),
        "broker_port must be integer, please check your mqtt_push element "
        "configuration file");
    brokerPort_ = portIt->get<int>();

    auto topicIt = configure.find(CONFIG_INTERNAL_TOPIC_FIELD);
    STREAM_CHECK(
        (topicIt != configure.end() && topicIt->is_string()),
        "topic must be string, please check your mqtt_push element "
        "configuration file");
    topic_ = topicIt->get<std::string>();

    auto clientIdIt = configure.find(CONFIG_INTERNAL_CLIENT_ID_FIELD);
    if (clientIdIt != configure.end() && clientIdIt->is_string()) {
      clientId_ = clientIdIt->get<std::string>();
    } else {
      clientId_ = "";  // mosquitto 自动生成
    }

  } while (false);
  return errorCode;
}

common::ErrorCode MqttPush::doWork(int dataPipeId) {
  std::vector<int> inputPorts = getInputPorts();
  int inputPort = inputPorts[0];
  int outputPort = 0;
  if (!getSinkElementFlag()) {
    std::vector<int> outputPorts = getOutputPorts();
    outputPort = outputPorts[0];
  }

  auto data = popInputData(inputPort, dataPipeId);
  while (!data && (getThreadStatus() == ThreadStatus::RUN)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    data = popInputData(inputPort, dataPipeId);
  }
  if (data == nullptr) return common::ErrorCode::SUCCESS;

  auto objectMetadata =
      std::static_pointer_cast<common::ObjectMetadata>(data);

  if (!objectMetadata->mFrame->mEndOfStream) {
    nlohmann::json serializedObj = serializeMetadata(objectMetadata);

    int channel_id = objectMetadata->mFrame->mChannelIdInternal;
    auto implIt = mapImpl_.find(channel_id);
    if (implIt == mapImpl_.end()) {
      std::lock_guard<std::mutex> lock(mapMtx);
      auto mqttImpl = std::make_shared<MqttPushImpl_>(
          brokerIp_, brokerPort_, topic_, clientId_, channel_id);
      mapImpl_[channel_id] = mqttImpl;
      mapImpl_[channel_id]->pushQueue(
          std::make_shared<nlohmann::json>(serializedObj));
    } else
      implIt->second->pushQueue(
          std::make_shared<nlohmann::json>(serializedObj));
  }

  int channel_id_internal = objectMetadata->mFrame->mChannelIdInternal;
  int outDataPipeId =
      getSinkElementFlag()
          ? 0
          : (channel_id_internal %
             getOutputConnectorCapacity(outputPort));
  common::ErrorCode errorCode =
      pushOutputData(outputPort, outDataPipeId,
                     std::static_pointer_cast<void>(objectMetadata));
  if (common::ErrorCode::SUCCESS != errorCode) {
    IVS_WARN(
        "Send data fail, element id: {0:d}, output port: {1:d}, data: "
        "{2:p}",
        getId(), outputPort, static_cast<void*>(objectMetadata.get()));
  }
  return common::ErrorCode::SUCCESS;
}

REGISTER_WORKER("mqtt_push", MqttPush)

}  // namespace mqtt_push
}  // namespace element
}  // namespace sophon_stream
