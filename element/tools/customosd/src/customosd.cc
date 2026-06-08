//===----------------------------------------------------------------------===//
//
// Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
// SOPHON-STREAM is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//

#include "customosd.h"

#include <sys/stat.h>

#include <fstream>

#include <opencv2/opencv.hpp>

#include "common/common_defs.h"
#include "common/logger.h"
#include "element_factory.h"

namespace sophon_stream {
namespace element {
namespace customosd {

namespace {

const std::vector<std::vector<int>> kColors = {
    {0, 0, 255},    {0, 255, 0},    {255, 0, 0},    {255, 255, 0},
    {255, 0, 255},  {0, 255, 255},  {128, 0, 255},  {255, 128, 0}};

std::vector<common::Point<int>> parsePolygon(const nlohmann::json& polygon) {
  std::vector<common::Point<int>> points;
  for (const auto& point : polygon) {
    STREAM_CHECK(point.is_object(), "roi/line point must be object");
    auto leftIt = point.find("left");
    auto topIt = point.find("top");
    STREAM_CHECK(leftIt != point.end() && leftIt->is_number_integer(),
                 "point.left must be int");
    STREAM_CHECK(topIt != point.end() && topIt->is_number_integer(),
                 "point.top must be int");
    points.push_back({leftIt->get<int>(), topIt->get<int>()});
  }
  return points;
}

}  // namespace

CustomOsd::CustomOsd() {}

CustomOsd::~CustomOsd() {}

bool CustomOsd::ensureSaveDir() {
  if (mSavePath.empty()) return false;  // 未配置 save_path 时不保存
  struct stat info;
  if (stat(mSavePath.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
    return true;
  }
  if (mkdir(mSavePath.c_str(), 0755) == 0) {
    IVS_INFO("CustomOsd created save directory: {}", mSavePath);
    return true;
  }
  IVS_ERROR("CustomOsd failed to create save directory: {}", mSavePath);
  return false;
}

common::ErrorCode CustomOsd::initInternal(const std::string& json) {
  common::ErrorCode errorCode = common::ErrorCode::SUCCESS;
  do {
    auto configure = nlohmann::json::parse(json, nullptr, false);
    if (!configure.is_object()) {
      errorCode = common::ErrorCode::PARSE_CONFIGURE_FAIL;
      break;
    }

    mFpsProfiler.config("fps_customosd", 100);
    mPutText = configure.value(CONFIG_INTERNAL_PUT_TEXT_FIELD, false);
    if (configure.contains(CONFIG_INTERNAL_SAVE_PATH_FIELD)) {
      mSavePath = configure[CONFIG_INTERNAL_SAVE_PATH_FIELD].get<std::string>();
    } else {
      mSavePath.clear();
    }

    auto classNamesIt = configure.find(CONFIG_INTERNAL_CLASS_NAMES_FIELD);
    if (classNamesIt != configure.end()) {
      std::ifstream istream(classNamesIt->get<std::string>());
      STREAM_CHECK(istream.good(), "class_names_file does not exist");
      std::string line;
      while (std::getline(istream, line)) {
        if (!line.empty()) mClassNames.push_back(line);
      }
    }

    auto channelsIt = configure.find(CONFIG_INTERNAL_CHANNELS_FIELD);
    STREAM_CHECK(channelsIt != configure.end() && channelsIt->is_array(),
                 "channels must be array in customosd config");

    for (const auto& channel_cfg : *channelsIt) {
      auto channelIdIt = channel_cfg.find(CONFIG_INTERNAL_CHANNEL_ID_FIELD);
      STREAM_CHECK(channelIdIt != channel_cfg.end() &&
                       channelIdIt->is_number_integer(),
                   "channel_id must be int");

      int channel_id = channelIdIt->get<int>();
      ChannelRule rule;

      auto roisIt = channel_cfg.find(CONFIG_INTERNAL_ROIS_FIELD);
      if (roisIt != channel_cfg.end() && roisIt->is_array()) {
        for (const auto& roi : *roisIt) {
          STREAM_CHECK(roi.is_array(), "each roi must be point array");
          auto polygon = parsePolygon(roi);
          STREAM_CHECK(polygon.size() >= 3,
                       "roi polygon must have at least 3 points");
          rule.rois.push_back(polygon);
        }
      }

      auto linesIt = channel_cfg.find(CONFIG_INTERNAL_LINES_FIELD);
      if (linesIt != channel_cfg.end() && linesIt->is_array()) {
        for (const auto& line : *linesIt) {
          STREAM_CHECK(line.is_array(), "each line must be point array");
          auto segment = parsePolygon(line);
          STREAM_CHECK(segment.size() == 2, "line must have exactly 2 points");
          rule.lines.push_back(segment);
        }
      }

      mChannelRules[channel_id] = rule;
      IVS_INFO("CustomOsd channel {} configured with {} rois, {} lines",
               channel_id, rule.rois.size(), rule.lines.size());
    }

    if (!mSavePath.empty()) {
      ensureSaveDir();
      IVS_INFO("CustomOsd save_path enabled: {}", mSavePath);
    } else {
      IVS_INFO("CustomOsd save_path is empty, line crossing images will not be saved");
    }
  } while (false);
  return errorCode;
}

void CustomOsd::filterByRoi(
    std::shared_ptr<common::ObjectMetadata> objectMetadata,
    const ChannelRule& rule) {
  if (rule.rois.empty()) return;

  std::vector<std::shared_ptr<common::DetectedObjectMetadata>>
      filtered_detections;
  std::vector<std::shared_ptr<common::TrackedObjectMetadata>> filtered_tracks;

  const size_t det_count = objectMetadata->mDetectedObjectMetadatas.size();
  for (size_t i = 0; i < det_count; ++i) {
    const auto& det = objectMetadata->mDetectedObjectMetadatas[i];
    common::Point<int> center = det->mBox.center();
    if (!isPointInAnyRoi(center, rule.rois)) continue;

    filtered_detections.push_back(det);
    if (i < objectMetadata->mTrackedObjectMetadatas.size()) {
      filtered_tracks.push_back(objectMetadata->mTrackedObjectMetadatas[i]);
    }
  }

  objectMetadata->mDetectedObjectMetadatas = filtered_detections;
  objectMetadata->mTrackedObjectMetadatas = filtered_tracks;
}

void CustomOsd::checkLineCrossing(
    std::shared_ptr<common::ObjectMetadata> objectMetadata,
    const ChannelRule& rule, cv::Mat& frame) {
  if (rule.lines.empty()) return;

  const int channel_id = objectMetadata->mFrame->mChannelId;
  bool need_save = false;

  {
    std::lock_guard<std::mutex> lk(mStateMtx);
    auto& channel_states = mTrackStates[channel_id];

    for (size_t i = 0; i < objectMetadata->mDetectedObjectMetadatas.size(); ++i) {
      if (i >= objectMetadata->mTrackedObjectMetadatas.size()) break;

      int track_id = objectMetadata->mTrackedObjectMetadatas[i]->mTrackId;
      common::Point<int> curr_center =
          objectMetadata->mDetectedObjectMetadatas[i]->mBox.center();
      auto& state = channel_states[track_id];

      if (state.has_prev) {
        for (size_t line_idx = 0; line_idx < rule.lines.size(); ++line_idx) {
          if (!isSegmentCrossingLine(state.prev_center, curr_center,
                                     rule.lines[line_idx])) {
            continue;
          }

          std::string save_key = std::to_string(channel_id) + "_" +
                                 std::to_string(track_id) + "_" +
                                 std::to_string(line_idx);
          if (mSavedCrossings.count(save_key) > 0) continue;

          mSavedCrossings.insert(save_key);
          need_save = true;
          IVS_INFO(
              "CustomOsd line crossing: channel={}, track={}, line={}, frame={}",
              channel_id, track_id, line_idx,
              objectMetadata->mFrame->mFrameId);
        }
      }

      state.prev_center = curr_center;
      state.has_prev = true;
    }
  }

  if (!need_save || mSavePath.empty()) return;
  if (!ensureSaveDir()) return;

  std::string img_file =
      mSavePath + "/channel_" + std::to_string(channel_id) + "_frame_" +
      std::to_string(objectMetadata->mFrame->mFrameId) + "_" +
      std::to_string(objectMetadata->mFrame->mTimestamp) + ".jpg";
  if (!cv::imwrite(img_file, frame)) {
    IVS_WARN("CustomOsd failed to save image: {}", img_file);
  } else {
    IVS_INFO("CustomOsd saved crossing image: {}", img_file);
  }
}

void CustomOsd::drawOverlays(const ChannelRule& rule, cv::Mat& frame) {
  for (const auto& roi : rule.rois) {
    if (roi.size() < 3) continue;
    std::vector<cv::Point> poly;
    poly.reserve(roi.size());
    for (const auto& pt : roi) {
      poly.emplace_back(pt.mX, pt.mY);
    }
    const cv::Point* pts = poly.data();
    int npts = static_cast<int>(poly.size());
    cv::polylines(frame, &pts, &npts, 1, true, cv::Scalar(0, 255, 0), 2,
                  cv::LINE_AA);
  }

  for (const auto& line : rule.lines) {
    if (line.size() != 2) continue;
    cv::line(frame, cv::Point(line[0].mX, line[0].mY),
             cv::Point(line[1].mX, line[1].mY), cv::Scalar(0, 0, 255), 2,
             cv::LINE_AA);
  }
}

void CustomOsd::drawTrackBoxes(
    std::shared_ptr<common::ObjectMetadata> objectMetadata, cv::Mat& frame) {
  const int colors_num = static_cast<int>(kColors.size());
  const int thickness = 2;
  const float font_scale = 0.7;

  for (size_t i = 0; i < objectMetadata->mDetectedObjectMetadatas.size(); ++i) {
    const auto& det = objectMetadata->mDetectedObjectMetadatas[i];
    int track_id = -1;
    if (i < objectMetadata->mTrackedObjectMetadatas.size()) {
      track_id = objectMetadata->mTrackedObjectMetadatas[i]->mTrackId;
    }

    const int color_idx =
        track_id >= 0 ? track_id % colors_num : static_cast<int>(i % colors_num);
    cv::Scalar color(kColors[color_idx][0], kColors[color_idx][1],
                     kColors[color_idx][2]);

    cv::rectangle(frame, cv::Point(det->mBox.mX, det->mBox.mY),
                  cv::Point(det->mBox.mX + det->mBox.mWidth,
                            det->mBox.mY + det->mBox.mHeight),
                  color, thickness);

    if (mPutText) {
      std::string label;
      if (track_id >= 0) {
        label = "id:" + std::to_string(track_id);
      }
      if (!mClassNames.empty() &&
          det->mClassify >= 0 &&
          det->mClassify < static_cast<int>(mClassNames.size())) {
        if (!label.empty()) label += " ";
        label += mClassNames[det->mClassify];
      }
      if (!label.empty()) {
        cv::putText(frame, label,
                    cv::Point(det->mBox.mX,
                              std::max(det->mBox.mY - 5, 0)),
                    cv::FONT_HERSHEY_SIMPLEX, font_scale, color, thickness);
      }
    }
  }
}

void CustomOsd::draw(std::shared_ptr<common::ObjectMetadata> objectMetadata) {
  const int channel_id = objectMetadata->mFrame->mChannelId;
  ChannelRule rule;
  auto rule_it = mChannelRules.find(channel_id);
  if (rule_it != mChannelRules.end()) {
    rule = rule_it->second;
  }

  filterByRoi(objectMetadata, rule);

  bm_image image = objectMetadata->mFrame->mSpDataOsd
                       ? *(objectMetadata->mFrame->mSpDataOsd)
                       : *(objectMetadata->mFrame->mSpData);

  cv::Mat frame_to_draw;
  cv::bmcv::toMAT(&image, frame_to_draw);

  drawOverlays(rule, frame_to_draw);
  drawTrackBoxes(objectMetadata, frame_to_draw);
  checkLineCrossing(objectMetadata, rule, frame_to_draw);

  std::shared_ptr<bm_image> image_storage(
      new bm_image, [](bm_image* img) {
        bm_image_destroy(*img);
        delete img;
      });

  cv::bmcv::toBMI(frame_to_draw, image_storage.get());
  if (image_storage->image_format != FORMAT_YUV420P) {
    bm_image frame;
    bm_image_create(objectMetadata->mFrame->mHandle, image_storage->height,
                    image_storage->width, FORMAT_YUV420P,
                    image_storage->data_type, &frame);
    auto ret =
        bm_image_alloc_dev_mem_heap_mask(frame, STREAM_VPU_HEAP_MASK);
    STREAM_CHECK(ret == 0, "Alloc Device Memory Failed! Program Terminated.");
    bmcv_image_storage_convert(objectMetadata->mFrame->mHandle, 1,
                               image_storage.get(), &frame);
    bm_image_destroy(*image_storage);
    *image_storage = frame;
  }

  objectMetadata->mFrame->mSpDataOsd = image_storage;
  objectMetadata->mFrame->mSpData.reset();
}

common::ErrorCode CustomOsd::doWork(int dataPipeId) {
  common::ErrorCode errorCode = common::ErrorCode::SUCCESS;

  std::vector<int> inputPorts = getInputPorts();
  int inputPort = inputPorts[0];
  int outputPort = 0;
  if (!getSinkElementFlag()) {
    std::vector<int> outputPorts = getOutputPorts();
    outputPort = outputPorts[0];
  }

  std::shared_ptr<void> data;
  while (getThreadStatus() == ThreadStatus::RUN) {
    data = popInputData(inputPort, dataPipeId);
    if (!data) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    break;
  }

  if (!data) return common::ErrorCode::SUCCESS;

  auto objectMetadata = std::static_pointer_cast<common::ObjectMetadata>(data);
  if (!(objectMetadata->mFrame->mEndOfStream) &&
      std::find(objectMetadata->mSkipElements.begin(),
                objectMetadata->mSkipElements.end(),
                getId()) == objectMetadata->mSkipElements.end()) {
    draw(objectMetadata);
    mFpsProfiler.add(1);
  }

  int channel_id_internal = objectMetadata->mFrame->mChannelIdInternal;
  int outDataPipeId =
      getSinkElementFlag()
          ? 0
          : (channel_id_internal % getOutputConnectorCapacity(outputPort));
  errorCode = pushOutputData(outputPort, outDataPipeId, objectMetadata);
  if (common::ErrorCode::SUCCESS != errorCode) {
    IVS_WARN(
        "Send data fail, element id: {0:d}, output port: {1:d}, data: "
        "{2:p}",
        getId(), outputPort, static_cast<void*>(objectMetadata.get()));
  }

  return common::ErrorCode::SUCCESS;
}

REGISTER_WORKER("customosd", CustomOsd)

}  // namespace customosd
}  // namespace element
}  // namespace sophon_stream
