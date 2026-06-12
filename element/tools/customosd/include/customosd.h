//===----------------------------------------------------------------------===//
//
// Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
// SOPHON-STREAM is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//

#ifndef SOPHON_STREAM_ELEMENT_CUSTOMOSD_H_
#define SOPHON_STREAM_ELEMENT_CUSTOMOSD_H_

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/common_defs.h"
#include "common/object_metadata.h"
#include "common/profiler.h"
#include "common/work_time_log.h"
#include "element.h"
#include "geometry_utils.h"

namespace cv {
class Mat;
}

namespace sophon_stream {
namespace element {
namespace customosd {

struct TrackCrossState {
  common::Point<int> prev_center;
  bool has_prev = false;
};

struct ChannelRule {
  std::vector<std::vector<common::Point<int>>> rois;
  std::vector<std::vector<common::Point<int>>> lines;
};

struct LatencyStats {
  int count = 0;
  std::int64_t sum_ms = 0;
};

enum class SaveImageMode { CLEAN, ANNOTATED };

class CustomOsd : public ::sophon_stream::framework::Element {
 public:
  CustomOsd();
  ~CustomOsd() override;

  common::ErrorCode initInternal(const std::string& json) override;
  common::ErrorCode doWork(int dataPipeId) override;

  static constexpr const char* CONFIG_INTERNAL_CLASS_NAMES_FIELD =
      "class_names_file";
  static constexpr const char* CONFIG_INTERNAL_PUT_TEXT_FIELD = "put_text";
  static constexpr const char* CONFIG_INTERNAL_SAVE_PATH_FIELD = "save_path";
  static constexpr const char* CONFIG_INTERNAL_SAVE_IMAGE_MODE_FIELD =
      "save_image_mode";
  static constexpr const char* CONFIG_INTERNAL_CHANNELS_FIELD = "channels";
  static constexpr const char* CONFIG_INTERNAL_CHANNEL_ID_FIELD = "channel_id";
  static constexpr const char* CONFIG_INTERNAL_ROIS_FIELD = "rois";
  static constexpr const char* CONFIG_INTERNAL_LINES_FIELD = "lines";
  static constexpr const char* CONFIG_INTERNAL_LEFT_FIELD = "left";
  static constexpr const char* CONFIG_INTERNAL_TOP_FIELD = "top";
  static constexpr const char* CONFIG_INTERNAL_DROP_INTERVAL_FIELD =
      "drop_interval";

 private:
  void filterByRoi(std::shared_ptr<common::ObjectMetadata> objectMetadata,
                   const ChannelRule& rule);
  bool checkLineCrossing(std::shared_ptr<common::ObjectMetadata> objectMetadata,
                         const ChannelRule& rule);
  void drawOverlaysBmcv(bm_handle_t handle, const ChannelRule& rule,
                        bm_image& frame, float scale_x, float scale_y);
  void drawTrackBoxesBmcv(bm_handle_t handle,
                          std::shared_ptr<common::ObjectMetadata> objectMetadata,
                          bm_image& frame, float scale_x, float scale_y);
  void drawOverlaysOpenCv(bm_handle_t handle, const ChannelRule& rule,
                          bm_image& frame);
  void drawTrackBoxesOpenCv(
      bm_handle_t handle,
      std::shared_ptr<common::ObjectMetadata> objectMetadata,
      bm_image& frame);
  void draw(std::shared_ptr<common::ObjectMetadata> objectMetadata);
  bool ensureSaveDir();
  bool saveCrossingImage(std::shared_ptr<common::ObjectMetadata> objectMetadata,
                         const cv::Mat& osd_frame, const cv::Mat& clean_frame);
  void recordOutputLatency(int channel_id, std::int64_t latency_ms);
  bool shouldDropFrame(int channel_id);

  static constexpr int LATENCY_LOG_INTERVAL = 100;

  std::vector<std::string> mClassNames;
  bool mPutText = false;
  std::string mSavePath;
  SaveImageMode mSaveImageMode = SaveImageMode::CLEAN;
  std::unordered_map<int, ChannelRule> mChannelRules;
  int mDropInterval = 1;

  std::mutex mStateMtx;
  std::unordered_map<int, int> mDropFrameCounters;

  // Device image buffer pool — reuses bm_image across frames to avoid
  // per-frame alloc/free of VPU heap memory.
  std::mutex mPoolMtx;
  std::unordered_map<uint64_t, std::vector<bm_image>> mBufferPool;
  bm_image allocateImage(bm_handle_t handle, int w, int h,
                         bm_image_format_ext fmt, bm_image_data_format_ext dtype);
  void recycleImage(bm_image img);
  std::mutex mLatencyMtx;
  std::unordered_map<int, std::unordered_map<int, TrackCrossState>> mTrackStates;
  std::unordered_set<std::string> mSavedCrossings;
  std::unordered_map<int, LatencyStats> mLatencyStats;
  ::sophon_stream::common::WorkTimeLogGate mWorkTimeLogGate;

  ::sophon_stream::common::FpsProfiler mFpsProfiler;
};

}  // namespace customosd
}  // namespace element
}  // namespace sophon_stream

#endif  // SOPHON_STREAM_ELEMENT_CUSTOMOSD_H_
