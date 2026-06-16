//===----------------------------------------------------------------------===//
//
// Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
// SOPHON-STREAM is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//

#include "websocket_push.h"

#include "common/common_defs.h"
#include "common/logger.h"
#include "common/serialize.h"
#include "element_factory.h"

namespace sophon_stream {
namespace element {
namespace websocket_push {

/**
 * @brief 序列化 ObjectMetadata 为 JSON，不包含图像 base64 数据
 *         合并 detected + tracked 为统一的 objects 数组
 */
static nlohmann::json serializeMetadata(
    const std::shared_ptr<common::ObjectMetadata>& obj) {
  nlohmann::json j;

  // 帧信息（不含图像数据）
  if (obj->mFrame) {
    j["mFrame"]["mChannelId"] = obj->mFrame->mChannelId;
    j["mFrame"]["mFrameId"] = obj->mFrame->mFrameId;
    j["mFrame"]["mTimestamp"] = obj->mFrame->mTimestamp;
    j["mFrame"]["mWidth"] = obj->mFrame->mWidth;
    j["mFrame"]["mHeight"] = obj->mFrame->mHeight;
    j["mChannelIdInternal"] = obj->mFrame->mChannelIdInternal;
  }

  // 合并 detected + tracked 为统一的 objects 数组
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

// ==================== WebSocketPushImpl_ ====================

WebSocketPushImpl_::WebSocketPushImpl_(std::string& ip, int port,
                                       std::string path, int channel)
    : ip(ip), port(port), path(path) {
  mUri = "ws://" + ip + ":" + std::to_string(port) + path;

  // 启动 websocket 事件循环线程
  mEventThread = std::thread(&WebSocketPushImpl_::connectLoop, this);
  // 启动消息发送线程
  workThread = std::thread(&WebSocketPushImpl_::postFunc, this);

  mFpsProfilerName = "websocket_push_ch" + std::to_string(channel) + "_fps";
  mFpsProfiler.config(mFpsProfilerName, 100);
}

void WebSocketPushImpl_::release() {
  isRunning = false;
  {
    std::lock_guard<std::mutex> lock(mConnectMtx);
    mConnected = false;
  }
  try {
    mClient.stop();
  } catch (...) {
  }
  if (mEventThread.joinable()) mEventThread.join();
  if (workThread.joinable()) workThread.join();
}

// ---- WebSocket 连接管理（运行于 mEventThread） ----

void WebSocketPushImpl_::onOpen(connection_hdl hdl) {
  {
    std::lock_guard<std::mutex> lock(mConnectMtx);
    mConnected = true;
    mHandle = hdl;
  }
  IVS_INFO("WebSocket connected to {0}", mUri);
}

void WebSocketPushImpl_::onFail(connection_hdl hdl) {
  {
    std::lock_guard<std::mutex> lock(mConnectMtx);
    mConnected = false;
  }
  IVS_WARN("WebSocket connection failed to {0}", mUri);
}

void WebSocketPushImpl_::onClose(connection_hdl hdl) {
  {
    std::lock_guard<std::mutex> lock(mConnectMtx);
    mConnected = false;
  }
  IVS_INFO("WebSocket connection closed to {0}", mUri);
}

void WebSocketPushImpl_::connectLoop() {
  // One-time setup — init_asio() can only be called once.
  mClient.clear_access_channels(websocketpp::log::alevel::all);
  mClient.clear_access_channels(websocketpp::log::alevel::frame_payload);
  mClient.init_asio();

  mClient.set_open_handler(
      websocketpp::lib::bind(&WebSocketPushImpl_::onOpen, this, _1));
  mClient.set_fail_handler(
      websocketpp::lib::bind(&WebSocketPushImpl_::onFail, this, _1));
  mClient.set_close_handler(
      websocketpp::lib::bind(&WebSocketPushImpl_::onClose, this, _1));

  // Interruptible sleep — polls isRunning every 100ms so shutdown is prompt.
  auto waitWhileRunning = [this](int seconds) {
    for (int i = 0; i < seconds * 10 && isRunning; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  };

  while (isRunning) {
    try {
      websocketpp::lib::error_code ec;
      auto con = mClient.get_connection(mUri, ec);
      if (ec) {
        IVS_WARN(
            "WebSocket get_connection error for {0}: {1}, retrying in 3s...",
            mUri, ec.message());
        waitWhileRunning(3);
        continue;
      }

      mHandle = con->get_handle();
      mClient.connect(con);

      // run() blocks until the connection closes or stop() is called.
      mClient.run();

      // Connection closed — wait before reconnecting.
      {
        std::lock_guard<std::mutex> lock(mConnectMtx);
        mConnected = false;
      }
      waitWhileRunning(3);
    } catch (const std::exception& e) {
      IVS_WARN("WebSocket error: {0}, retrying in 3s...", e.what());
      {
        std::lock_guard<std::mutex> lock(mConnectMtx);
        mConnected = false;
      }
      waitWhileRunning(3);
    }
  }
}

// ---- 消息发送（运行于 workThread） ----

void WebSocketPushImpl_::postFunc() {
  while (isRunning) {
    auto ptr = popQueue();
    if (ptr == nullptr) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    bool connected;
    {
      std::lock_guard<std::mutex> lock(mConnectMtx);
      connected = mConnected;
    }

    if (!connected) {
      // 重新入队等待连接
      std::lock_guard<std::mutex> lock(mtx);
      if (objQueue.size() < maxQueueLen) {
        objQueue.push(ptr);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    mFpsProfiler.add(1);
    try {
      mClient.send(mHandle, ptr->dump(), websocketpp::frame::opcode::text);
    } catch (const std::exception& e) {
      IVS_WARN("WebSocket send error: {0}", e.what());
      {
        std::lock_guard<std::mutex> lock(mConnectMtx);
        mConnected = false;
      }
      // 重新入队
      std::lock_guard<std::mutex> lock(mtx);
      if (objQueue.size() < maxQueueLen) {
        objQueue.push(ptr);
      }
    }
  }
}

// ---- 队列操作 ----

bool WebSocketPushImpl_::pushQueue(std::shared_ptr<nlohmann::json> j) {
  size_t len = getQueueSize();
  if (len >= maxQueueLen) return false;
  {
    std::lock_guard<std::mutex> lock(mtx);
    objQueue.push(j);
  }
  return true;
}

std::shared_ptr<nlohmann::json> WebSocketPushImpl_::popQueue() {
  std::lock_guard<std::mutex> lock(mtx);
  std::shared_ptr<nlohmann::json> j = nullptr;
  if (objQueue.empty()) return j;
  j = objQueue.front();
  objQueue.pop();
  return j;
}

size_t WebSocketPushImpl_::getQueueSize() {
  size_t len = 0;
  {
    std::lock_guard<std::mutex> lock(mtx);
    len = objQueue.size();
  }
  return len;
}

// ==================== WebSocketPush ====================

WebSocketPush::WebSocketPush() {}
WebSocketPush::~WebSocketPush() {
  for (auto [k, v] : mapImpl_) {
    v->release();
  }
}

common::ErrorCode WebSocketPush::initInternal(const std::string& json) {
  common::ErrorCode errorCode = common::ErrorCode::SUCCESS;
  do {
    auto configure = nlohmann::json::parse(json, nullptr, false);
    if (!configure.is_object()) {
      errorCode = common::ErrorCode::PARSE_CONFIGURE_FAIL;
      break;
    }

    mEnable = configure.value(CONFIG_INTERNAL_ENABLE_FIELD, true);
    IVS_INFO("WebSocketPush enable={}", mEnable);
    if (!mEnable) break;  // 未启用时跳过连接配置校验

    auto ipIt = configure.find(CONFIG_INTERNAL_IP_FIELD);
    STREAM_CHECK(
        (ipIt != configure.end() && ipIt->is_string()),
        "IP must be std::string, please check your websocket_push element "
        "configuration file");
    ip_ = ipIt->get<std::string>();
    auto portIt = configure.find(CONFIG_INTERNAL_PORT_FIELD);
    STREAM_CHECK(
        (portIt != configure.end() && portIt->is_number_integer()),
        "Port must be integer, please check your websocket_push element "
        "configuration file");
    port_ = portIt->get<int>();

    auto pathIt = configure.find(CONFIG_INTERNAL_PATH_FIELD);
    STREAM_CHECK(
        (pathIt != configure.end() && pathIt->is_string()),
        "Path must be string, please check your websocket_push element "
        "configuration file");
    path_ = pathIt->get<std::string>();

  } while (false);
  return errorCode;
}

common::ErrorCode WebSocketPush::doWork(int dataPipeId) {
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

  if (mEnable && !objectMetadata->mFrame->mEndOfStream) {
    // 仅序列化元数据，不包含图像 base64
    nlohmann::json serializedObj = serializeMetadata(objectMetadata);

    int channel_id = objectMetadata->mFrame->mChannelIdInternal;
    auto implIt = mapImpl_.find(channel_id);
    if (implIt == mapImpl_.end()) {
      std::lock_guard<std::mutex> lock(mapMtx);
      auto wsImpl = std::make_shared<WebSocketPushImpl_>(ip_, port_, path_,
                                                         channel_id);
      mapImpl_[channel_id] = wsImpl;
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

REGISTER_WORKER("websocket_push", WebSocketPush)

}  // namespace websocket_push
}  // namespace element
}  // namespace sophon_stream
