//===----------------------------------------------------------------------===//
//
// Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
// SOPHON-STREAM is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//

#ifndef SOPHON_STREAM_ELEMENT_WEBSOCKET_PUSH_H_
#define SOPHON_STREAM_ELEMENT_WEBSOCKET_PUSH_H_

#include <mutex>
#include <nlohmann/json.hpp>
#include <queue>
#include <string>

#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>

#include "common/object_metadata.h"
#include "common/profiler.h"
#include "element.h"

namespace sophon_stream {
namespace element {
namespace websocket_push {

using WSClient = websocketpp::client<websocketpp::config::asio_client>;
using websocketpp::connection_hdl;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;

class WebSocketPushImpl_ {
 public:
  WebSocketPushImpl_(std::string& ip, int port, std::string path, int channel);
  bool pushQueue(std::shared_ptr<nlohmann::json> j);
  void release();

 private:
  std::queue<std::shared_ptr<nlohmann::json>> objQueue;
  std::thread workThread;
  void postFunc();
  void connectLoop();
  void onOpen(connection_hdl hdl);
  void onFail(connection_hdl hdl);
  void onClose(connection_hdl hdl);
  bool isRunning = true;

  std::shared_ptr<nlohmann::json> popQueue();
  size_t getQueueSize();
  std::mutex mtx;
  constexpr static int maxQueueLen = 20;

  WSClient mClient;
  connection_hdl mHandle;
  std::string mUri;
  bool mConnected = false;
  std::mutex mConnectMtx;
  std::thread mEventThread;

  std::string ip;
  int port;
  std::string path;

  std::string mFpsProfilerName;
  ::sophon_stream::common::FpsProfiler mFpsProfiler;
};

class WebSocketPush : public ::sophon_stream::framework::Element {
 public:
  WebSocketPush();
  ~WebSocketPush() override;

  common::ErrorCode initInternal(const std::string& json) override;

  common::ErrorCode doWork(int dataPipeId) override;

  static constexpr const char* CONFIG_INTERNAL_IP_FIELD = "ip";
  static constexpr const char* CONFIG_INTERNAL_PORT_FIELD = "port";
  static constexpr const char* CONFIG_INTERNAL_PATH_FIELD = "path";
  static constexpr const char* CONFIG_INTERNAL_ENABLE_FIELD = "enable";

 private:
  std::unordered_map<int, std::shared_ptr<WebSocketPushImpl_>> mapImpl_;
  std::mutex mapMtx;
  std::string ip_;
  int port_;
  std::string path_;
  bool mEnable = true;
};

}  // namespace websocket_push
}  // namespace element
}  // namespace sophon_stream

#endif
