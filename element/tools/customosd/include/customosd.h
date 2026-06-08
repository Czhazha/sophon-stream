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

#include "common/object_metadata.h"
#include "common/profiler.h"
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
  static constexpr const char* CONFIG_INTERNAL_CHANNELS_FIELD = "channels";
  static constexpr const char* CONFIG_INTERNAL_CHANNEL_ID_FIELD = "channel_id";
  static constexpr const char* CONFIG_INTERNAL_ROIS_FIELD = "rois";
  static constexpr const char* CONFIG_INTERNAL_LINES_FIELD = "lines";
  static constexpr const char* CONFIG_INTERNAL_LEFT_FIELD = "left";
  static constexpr const char* CONFIG_INTERNAL_TOP_FIELD = "top";

 private:
  void filterByRoi(std::shared_ptr<common::ObjectMetadata> objectMetadata,
                   const ChannelRule& rule);
  void checkLineCrossing(std::shared_ptr<common::ObjectMetadata> objectMetadata,
                         const ChannelRule& rule, cv::Mat& frame);
  void drawOverlays(const ChannelRule& rule, cv::Mat& frame);
  void drawTrackBoxes(std::shared_ptr<common::ObjectMetadata> objectMetadata,
                      cv::Mat& frame);
  void draw(std::shared_ptr<common::ObjectMetadata> objectMetadata);
  bool ensureSaveDir();

  std::vector<std::string> mClassNames;
  bool mPutText = false;
  std::string mSavePath;
  std::unordered_map<int, ChannelRule> mChannelRules;

  std::mutex mStateMtx;
  std::unordered_map<int, std::unordered_map<int, TrackCrossState>> mTrackStates;
  std::unordered_set<std::string> mSavedCrossings;

  ::sophon_stream::common::FpsProfiler mFpsProfiler;
};

}  // namespace customosd
}  // namespace element
}  // namespace sophon_stream

#endif  // SOPHON_STREAM_ELEMENT_CUSTOMOSD_H_
