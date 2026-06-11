// ===----------------------------------------------------------------------===
//
//  Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
//  SOPHON-DEMO is licensed under the 2-Clause BSD License except for the
//  third-party components.
//
// ===----------------------------------------------------------------------===

#include "qt_display.h"

#include <QMetaObject>
#include <QScreen>

#include <chrono>

#include "common/logger.h"
#include "element_factory.h"

namespace sophon_stream {
namespace element {
namespace qt_display {

namespace {

double elapsed_ms(const std::chrono::steady_clock::time_point& start,
                  const std::chrono::steady_clock::time_point& end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

}  // namespace

QtDisplay::QtDisplay() : qapp(nullptr), qwidget_ptr(nullptr), layout(nullptr) {}

void QtDisplay::requestQtQuit() {
  if (qapp) {
    QMetaObject::invokeMethod(qapp, "quit", Qt::QueuedConnection);
  }
}

void QtDisplay::shutdownQt() {
  if (!qt_thread.joinable()) return;
  if (qapp) {
    // Only break the event loop. Destroying QApplication/QWidget here unloads
    // the linuxfb/fl2000 platform plugin and segfaults; let the process exit
    // reclaim the memory instead.
    QMetaObject::invokeMethod(qapp, "quit", Qt::QueuedConnection);
  }
  qt_thread.join();
}

void QtDisplay::onStop() {
  std::call_once(qt_shutdown_once_, [this]() {
    IVS_INFO("Qt display stopping, element id: {0:d}", getId());
    shutdownQt();
    IVS_INFO("Qt display stopped, element id: {0:d}", getId());
  });
}

QtDisplay::~QtDisplay() {
  std::call_once(qt_shutdown_once_, [this]() { shutdownQt(); });
  for (auto& [k, v] : mFpsProfilers) {
    delete v;
  }
}

int QtDisplay::qt_func() {
  int q_argc = 1;
  char* q_argv = {"main"};

  qapp = new QApplication(q_argc, &q_argv);
  qwidget_ptr = new QWidget;

  qwidget_ptr->setFixedSize(screen_width, screen_height);
  qwidget_ptr->move(0, 0);
  layout = new QGridLayout(qwidget_ptr);
  // Default QGridLayout margins/spacing (~11px + 6px gaps) make a 2x3 grid
  // wider than screen_width, causing cell overlap and right-edge clipping.
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  int label_width = screen_width / cols;
  int label_height = screen_height / rows;

  for (int row = 0; row < rows; row++) {
    for (int col = 0; col < cols; col++) {
      std::shared_ptr<BMLabel> label_ptr =
          std::make_shared<BMLabel>(qwidget_ptr, label_width, label_height);
      layout->addWidget(label_ptr.get(), row, col);
      label_vec.push_back(label_ptr);
    }
  }

  qwidget_ptr->setLayout(layout);
  qwidget_ptr->show();

  {
    std::lock_guard<std::mutex> lock(ui_mutex);
    ui_ready = true;
  }
  ui_cv.notify_all();

  // Intentionally do not delete qapp/qwidget after exec() returns: tearing down
  // the linuxfb/fl2000 platform plugin crashes. The process exits right after.
  return qapp->exec();
}

common::ErrorCode QtDisplay::initInternal(const std::string& json) {
  auto configure = nlohmann::json::parse(json, nullptr, false);
  if (!configure.is_object()) return common::ErrorCode::PARSE_CONFIGURE_FAIL;

  screen_width = configure.find(CONFIG_INTERNAL_SCREEN_WIDTH)->get<int>();
  screen_height = configure.find(CONFIG_INTERNAL_SCREEN_HEIGHT)->get<int>();

  rows = configure.find(CONFIG_INTERNAL_ROWS)->get<int>();
  cols = configure.find(CONFIG_INTERNAL_COLS)->get<int>();
  stopped_num = 0;
  ui_ready = false;

  qt_thread = std::thread(&QtDisplay::qt_func, this);
  {
    std::unique_lock<std::mutex> lock(ui_mutex);
    ui_cv.wait_for(lock, std::chrono::seconds(10),
                   [this] { return ui_ready.load(); });
  }
  if (!ui_ready.load()) {
    IVS_WARN("qt display ui init timeout");
    return common::ErrorCode::UNKNOWN;
  }

  thread_num = getThreadNumber();

  return common::ErrorCode::SUCCESS;
}

common::ErrorCode QtDisplay::doWork(int dataPipeId) {
  const auto do_work_start = std::chrono::steady_clock::now();

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

  // Drop stale frames and keep only the latest one to limit device memory usage.
  while (getThreadStatus() == ThreadStatus::RUN) {
    auto newer = popInputData(inputPort, dataPipeId);
    if (!newer) break;
    data = newer;
  }

  auto objectMetadata = std::static_pointer_cast<common::ObjectMetadata>(data);

  int channel_id = objectMetadata->mFrame->mChannelIdInternal;
  bool should_log = false;
  if (!objectMetadata->mFrame->mEndOfStream) {
    should_log = mWorkTimeLogGate.tick(channel_id);
    if (should_log) {
      IVS_INFO("QtDisplay doWork start: channel={0:d}, dataPipeId={1:d}, time={2}",
               channel_id, dataPipeId, common::formatTimeOfDayMs());
    }
  }
  int label_idx = 0;
  {
    std::unique_lock<std::mutex> lock(channel_mutex);
    auto it = channel_id_to_label_idx.find(channel_id);
    if (it == channel_id_to_label_idx.end()) {
      label_idx = static_cast<int>(channel_id_to_label_idx.size());
      channel_id_to_label_idx[channel_id] = label_idx;
      channel_ids.insert(channel_id);
    } else {
      label_idx = it->second;
    }
  }
  auto bmimg_ptr = objectMetadata->mFrame->mSpDataOsd;

  if (bmimg_ptr == nullptr) bmimg_ptr = objectMetadata->mFrame->mSpData;

  if (objectMetadata->mFrame->mEndOfStream) {
    stopped_num++;
  } else {
    if (!ui_ready.load()) {
      std::unique_lock<std::mutex> lock(ui_mutex);
      ui_cv.wait_for(lock, std::chrono::seconds(10),
                     [this] { return ui_ready.load(); });
    }
    if (label_idx < static_cast<int>(label_vec.size())) {
      if (bmimg_ptr) {
        if (!mFpsProfilers.count(channel_id)) {
          auto* fps_profiler = new ::sophon_stream::common::FpsProfiler();
          mFpsProfilers[channel_id] = fps_profiler;
          mFpsProfilers[channel_id]->config(
              "qt_display_" + std::to_string(channel_id), 100);
        }
        mFpsProfilers[channel_id]->add(1);
        const float tmp_fps = mFpsProfilers[channel_id]->getTmpFps();
        // Conversion happens synchronously on this worker thread; the GUI
        // thread only does the lightweight setPixmap/update.
        label_vec[label_idx]->submit_frame(bmimg_ptr, tmp_fps);
      }
    } else
      IVS_WARN(
          "label index {0:d} exceeds label count {1:d} for channel {2:d}",
          label_idx, label_vec.size(), channel_id);
  }

  if (stopped_num == channel_ids.size() && qapp) {
    QMetaObject::invokeMethod(qapp, "quit", Qt::QueuedConnection);
  }

  int channel_id_internal = objectMetadata->mFrame->mChannelIdInternal;
  int outDataPipeId =
      getSinkElementFlag()
          ? 0
          : (channel_id_internal % getOutputConnectorCapacity(outputPort));
  common::ErrorCode errorCode =
      pushOutputData(outputPort, outDataPipeId,
                     std::static_pointer_cast<void>(objectMetadata));
  if (common::ErrorCode::SUCCESS != errorCode) {
    IVS_WARN(
        "Send data fail, element id: {0:d}, output port: {1:d}, data: "
        "{2:p}",
        getId(), outputPort, static_cast<void*>(objectMetadata.get()));
  }

  if (should_log) {
    const auto do_work_end = std::chrono::steady_clock::now();
    const double do_work_cost_ms =
        std::chrono::duration<double, std::milli>(do_work_end - do_work_start)
            .count();
    IVS_INFO(
        "QtDisplay doWork end: channel={0:d}, dataPipeId={1:d}, cost={2:.2f} "
        "ms, time={3}",
        channel_id, dataPipeId, do_work_cost_ms, common::formatTimeOfDayMs());
  }

  return common::ErrorCode::SUCCESS;
}

REGISTER_WORKER("qt_display", QtDisplay)

}  // namespace qt_display
}  // namespace element
}  // namespace sophon_stream

int main() {}