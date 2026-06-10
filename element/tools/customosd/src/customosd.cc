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
#include <sys/time.h>

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
      auto modeIt = configure.find(CONFIG_INTERNAL_SAVE_IMAGE_MODE_FIELD);
      if (modeIt != configure.end() && modeIt->is_string()) {
        const std::string mode = modeIt->get<std::string>();
        if (mode == "annotated") {
          mSaveImageMode = SaveImageMode::ANNOTATED;
        } else if (mode == "clean") {
          mSaveImageMode = SaveImageMode::CLEAN;
        } else {
          IVS_WARN(
              "CustomOsd unknown save_image_mode '{}', fallback to clean",
              mode);
          mSaveImageMode = SaveImageMode::CLEAN;
        }
      }
      IVS_INFO("CustomOsd save_path enabled: {}, save_image_mode: {}",
               mSavePath,
               mSaveImageMode == SaveImageMode::ANNOTATED ? "annotated"
                                                          : "clean");
    } else {
      mSaveImageMode = SaveImageMode::CLEAN;
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

bool CustomOsd::checkLineCrossing(
    std::shared_ptr<common::ObjectMetadata> objectMetadata,
    const ChannelRule& rule) {
  if (rule.lines.empty()) return false;

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

  return need_save && !mSavePath.empty();
}

bool CustomOsd::saveCrossingImage(
    std::shared_ptr<common::ObjectMetadata> objectMetadata,
    const cv::Mat& osd_frame, const cv::Mat& clean_frame) {
  if (!ensureSaveDir()) return false;

  const cv::Mat& source =
      mSaveImageMode == SaveImageMode::ANNOTATED ? osd_frame : clean_frame;
  if (source.empty()) {
    IVS_WARN("CustomOsd crossing save skipped, empty frame for channel={}, frame={}",
             objectMetadata->mFrame->mChannelId,
             objectMetadata->mFrame->mFrameId);
    return false;
  }

  const int channel_id = objectMetadata->mFrame->mChannelId;
  std::string img_file =
      mSavePath + "/channel_" + std::to_string(channel_id) + "_frame_" +
      std::to_string(objectMetadata->mFrame->mFrameId) + "_" +
      std::to_string(objectMetadata->mFrame->mTimestamp) + ".jpg";

  if (!cv::imwrite(img_file, source)) {
    IVS_WARN("CustomOsd failed to save image: {}", img_file);
    return false;
  }

  IVS_INFO("CustomOsd saved crossing image: {}", img_file);
  return true;
}

void CustomOsd::drawOverlays(const ChannelRule& rule, cv::Mat& frame,
                             float scale_x, float scale_y) {
  for (const auto& roi : rule.rois) {
    if (roi.size() < 3) continue;
    std::vector<cv::Point> poly;
    poly.reserve(roi.size());
    for (const auto& pt : roi) {
      poly.emplace_back(static_cast<int>(pt.mX * scale_x),
                        static_cast<int>(pt.mY * scale_y));
    }
    const cv::Point* pts = poly.data();
    int npts = static_cast<int>(poly.size());
    cv::polylines(frame, &pts, &npts, 1, true, cv::Scalar(0, 255, 0), 2,
                  cv::LINE_AA);
  }

  for (const auto& line : rule.lines) {
    if (line.size() != 2) continue;
    cv::line(frame,
             cv::Point(static_cast<int>(line[0].mX * scale_x),
                       static_cast<int>(line[0].mY * scale_y)),
             cv::Point(static_cast<int>(line[1].mX * scale_x),
                       static_cast<int>(line[1].mY * scale_y)),
             cv::Scalar(0, 0, 255), 2, cv::LINE_AA);
  }
}

void CustomOsd::drawTrackBoxes(
    std::shared_ptr<common::ObjectMetadata> objectMetadata, cv::Mat& frame,
    float scale_x, float scale_y) {
  const int colors_num = static_cast<int>(kColors.size());
  const int thickness = std::max(1, static_cast<int>(2 * scale_x));
  const float font_scale = 0.7f * scale_x;

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

    const int box_x = static_cast<int>(det->mBox.mX * scale_x);
    const int box_y = static_cast<int>(det->mBox.mY * scale_y);
    const int box_w = static_cast<int>(det->mBox.mWidth * scale_x);
    const int box_h = static_cast<int>(det->mBox.mHeight * scale_y);

    cv::rectangle(frame, cv::Point(box_x, box_y),
                  cv::Point(box_x + box_w, box_y + box_h), color, thickness);

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
                    cv::Point(box_x, std::max(box_y - 5, 0)),
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

  const bool need_save = checkLineCrossing(objectMetadata, rule);

  cv::Mat clean_frame;
  if (need_save && mSaveImageMode == SaveImageMode::CLEAN) {
    clean_frame = frame_to_draw.clone();
  }

  drawOverlays(rule, frame_to_draw);
  drawTrackBoxes(objectMetadata, frame_to_draw);

  if (need_save) {
    saveCrossingImage(objectMetadata, frame_to_draw, clean_frame);
  }

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

void CustomOsd::recordOutputLatency(int channel_id, std::int64_t latency_ms) {
  std::lock_guard<std::mutex> lk(mLatencyMtx);
  auto& stats = mLatencyStats[channel_id];
  stats.sum_ms += latency_ms;
  ++stats.count;
  if (stats.count < LATENCY_LOG_INTERVAL) {
    return;
  }

  IVS_INFO(
      "CustomOsd output latency avg: channel={}, frames={}, "
      "decode_to_output={}ms",
      channel_id, stats.count, stats.sum_ms / stats.count);
  stats.count = 0;
  stats.sum_ms = 0;
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
  const bool should_process =
      !(objectMetadata->mFrame->mEndOfStream) &&
      std::find(objectMetadata->mSkipElements.begin(),
                objectMetadata->mSkipElements.end(),
                getId()) == objectMetadata->mSkipElements.end();

  bool should_log = false;
  int channel_id = -1;
  std::string start_time;
  if (should_process) {
    channel_id = objectMetadata->mFrame->mChannelId;
    start_time = common::formatTimeOfDayMs();
    should_log = mWorkTimeLogGate.tick(channel_id);
    if (should_log) {
      IVS_INFO("CustomOsd doWork start: channel={}, time={}", channel_id,
               start_time);
    }
  }

  if (should_process) {
    draw(objectMetadata);
    mFpsProfiler.add(1);

    struct timeval tv_now;
    gettimeofday(&tv_now, nullptr);
    const std::int64_t output_time_us =
        static_cast<std::int64_t>(tv_now.tv_sec) * 1000000LL +
        tv_now.tv_usec;
    const std::int64_t decode_time_us = objectMetadata->mFrame->mTimestamp;
    const std::int64_t latency_ms = (output_time_us - decode_time_us) / 1000;
    recordOutputLatency(objectMetadata->mFrame->mChannelId, latency_ms);
  }

  if (should_log) {
    IVS_INFO("CustomOsd doWork end: channel={}, time={}", channel_id,
             common::formatTimeOfDayMs());
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
