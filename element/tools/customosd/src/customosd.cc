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

#include <cstring>
#include <fstream>
#include <map>

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

constexpr int kOsdDrawMaxWidth = 1920;
constexpr int kOsdDrawMaxHeight = 1080;

// Compact 64-bit key for pooling: | width (16) | height (16) | format (16) | dtype (16) |
inline uint64_t makePoolKey(int w, int h, int fmt, int dtype) {
  return (static_cast<uint64_t>(w) << 48) | (static_cast<uint64_t>(h) << 32) |
         (static_cast<uint64_t>(fmt & 0xFFFF) << 16) |
         static_cast<uint64_t>(dtype & 0xFFFF);
}

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

void writeMatToBmImage(bm_handle_t handle, cv::Mat& mat, bm_image& frame) {
  bm_image temp;
  cv::bmcv::toBMI(mat, &temp);

  // Create a new device-side image from the cv::Mat content.
  bm_image new_frame;
  bm_image_create(handle, temp.height, temp.width, FORMAT_YUV420P,
                  temp.data_type, &new_frame);
  auto ret =
      bm_image_alloc_dev_mem_heap_mask(new_frame, STREAM_VPU_HEAP_MASK);
  STREAM_CHECK(ret == 0, "Alloc Device Memory Failed! Program Terminated.");
  bmcv_image_storage_convert(handle, 1, &temp, &new_frame);
  bm_image_destroy(temp);

  // Replace the caller's bm_image with the new device-side copy.
  // bm_image is a C struct without a destructor, so member-wise copy is safe
  // and the caller takes ownership of new_frame's device memory.
  bm_image_destroy(frame);
  frame = new_frame;
}

}  // namespace

CustomOsd::CustomOsd() {}

CustomOsd::~CustomOsd() {
  for (auto& [key, vec] : mBufferPool) {
    for (auto& img : vec) {
      bm_image_destroy(img);
    }
  }
  mBufferPool.clear();
}

bm_image CustomOsd::allocateImage(bm_handle_t handle, int w, int h,
                                   bm_image_format_ext fmt,
                                   bm_image_data_format_ext dtype) {
  uint64_t key = makePoolKey(w, h, static_cast<int>(fmt), static_cast<int>(dtype));
  {
    std::lock_guard<std::mutex> lk(mPoolMtx);
    auto it = mBufferPool.find(key);
    if (it != mBufferPool.end() && !it->second.empty()) {
      bm_image img = it->second.back();
      it->second.pop_back();
      return img;
    }
  }
  bm_image img;
  bm_image_create(handle, h, w, fmt, dtype, &img);
  auto ret = bm_image_alloc_dev_mem_heap_mask(img, STREAM_VPU_HEAP_MASK);
  STREAM_CHECK(ret == 0, "Alloc Device Memory Failed! Program Terminated.");
  return img;
}

void CustomOsd::recycleImage(bm_image img) {
  uint64_t key = makePoolKey(img.width, img.height,
                              static_cast<int>(img.image_format),
                              static_cast<int>(img.data_type));
  std::lock_guard<std::mutex> lk(mPoolMtx);
  mBufferPool[key].push_back(img);
}

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
    mDropInterval = configure.value(CONFIG_INTERNAL_DROP_INTERVAL_FIELD, 1);
    STREAM_CHECK(mDropInterval >= 0,
                 "drop_interval must be >= 0 in customosd config");
    IVS_INFO(
        "CustomOsd drop_interval={} (0/1=keep all, N>1 keep only every Nth "
        "frame)",
        mDropInterval);
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
      IVS_INFO("CustomOsd save_path enabled: {}, crossing images will be saved "
               "with line-crossing targets boxed",
               mSavePath);
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

std::unordered_set<size_t> CustomOsd::checkLineCrossing(
    std::shared_ptr<common::ObjectMetadata> objectMetadata,
    const ChannelRule& rule, bool& need_save) {
  need_save = false;
  std::unordered_set<size_t> crossing_indices;
  if (rule.lines.empty()) return crossing_indices;

  const int channel_id = objectMetadata->mFrame->mChannelId;

  {
    std::lock_guard<std::mutex> lk(mStateMtx);
    auto& channel_states = mTrackStates[channel_id];

    for (size_t i = 0; i < objectMetadata->mDetectedObjectMetadatas.size(); ++i) {
      if (i >= objectMetadata->mTrackedObjectMetadatas.size()) break;

      int track_id = objectMetadata->mTrackedObjectMetadatas[i]->mTrackId;
      const common::Rectangle<int>& curr_box =
          objectMetadata->mDetectedObjectMetadatas[i]->mBox;
      auto& state = channel_states[track_id];

      bool is_crossing = false;
      for (size_t line_idx = 0; line_idx < rule.lines.size(); ++line_idx) {
        if (!isRectIntersectingLine(curr_box, rule.lines[line_idx])) continue;
        is_crossing = true;

        if (state.has_prev) {
          if (isRectIntersectingLine(state.prev_box, rule.lines[line_idx])) {
            // Already touching in the previous frame — not a new crossing.
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

      if (is_crossing) {
        crossing_indices.insert(i);
      }

      state.prev_box = curr_box;
      state.has_prev = true;
    }
  }

  if (mSavePath.empty()) {
    need_save = false;
  }

  return crossing_indices;
}

bool CustomOsd::saveCrossingImage(
    std::shared_ptr<common::ObjectMetadata> objectMetadata,
    const cv::Mat& frame) {
  if (!ensureSaveDir()) return false;

  if (frame.empty()) {
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

  if (!cv::imwrite(img_file, frame)) {
    IVS_WARN("CustomOsd failed to save image: {}", img_file);
    return false;
  }

  IVS_INFO("CustomOsd saved crossing image: {}", img_file);
  return true;
}

void CustomOsd::drawOverlaysBmcv(bm_handle_t handle, const ChannelRule& rule,
                                 bm_image& frame, float scale_x,
                                 float scale_y) {
  const bmcv_color_t roi_color{0, 255, 0};
  const bmcv_color_t line_color{255, 0, 0};
  const int thickness = std::max(1, static_cast<int>(2 * scale_x));

  // Collect all ROI polygon edges into one batch (all green).
  std::vector<bmcv_point_t> roi_starts, roi_ends;
  for (const auto& roi : rule.rois) {
    if (roi.size() < 3) continue;
    for (size_t i = 0; i < roi.size(); ++i) {
      size_t next = (i + 1) % roi.size();
      roi_starts.push_back({static_cast<int>(roi[i].mX * scale_x),
                            static_cast<int>(roi[i].mY * scale_y)});
      roi_ends.push_back({static_cast<int>(roi[next].mX * scale_x),
                          static_cast<int>(roi[next].mY * scale_y)});
    }
  }
  if (!roi_starts.empty()) {
    if (BM_SUCCESS != bmcv_image_draw_lines(handle, frame, roi_starts.data(),
                                            roi_ends.data(),
                                            static_cast<int>(roi_starts.size()),
                                            roi_color, thickness)) {
      IVS_WARN("CustomOsd bmcv draw roi lines failed");
    }
  }

  // Collect all tripwire segments into one batch (all red).
  std::vector<bmcv_point_t> line_starts, line_ends;
  for (const auto& line : rule.lines) {
    if (line.size() != 2) continue;
    line_starts.push_back({static_cast<int>(line[0].mX * scale_x),
                           static_cast<int>(line[0].mY * scale_y)});
    line_ends.push_back({static_cast<int>(line[1].mX * scale_x),
                         static_cast<int>(line[1].mY * scale_y)});
  }
  if (!line_starts.empty()) {
    if (BM_SUCCESS != bmcv_image_draw_lines(handle, frame, line_starts.data(),
                                            line_ends.data(),
                                            static_cast<int>(line_starts.size()),
                                            line_color, thickness)) {
      IVS_WARN("CustomOsd bmcv draw trip lines failed");
    }
  }
}

void CustomOsd::drawTrackBoxesBmcv(
    bm_handle_t handle, std::shared_ptr<common::ObjectMetadata> objectMetadata,
    bm_image& frame, float scale_x, float scale_y,
    const std::unordered_set<size_t>* filter_indices) {
  const int colors_num = static_cast<int>(kColors.size());
  const int thickness = std::max(1, static_cast<int>(6 * scale_x));
  const float font_scale = 3.0f * scale_x;

  // Single color for all boxes — one BMCV call per frame.
  const auto& bgr = kColors[0];
  std::vector<bmcv_rect_t> rects;
  rects.reserve(objectMetadata->mDetectedObjectMetadatas.size());
  for (size_t i = 0; i < objectMetadata->mDetectedObjectMetadatas.size(); ++i) {
    if (filter_indices && filter_indices->count(i) == 0) continue;
    const auto& det = objectMetadata->mDetectedObjectMetadatas[i];
    bmcv_rect_t rect;
    rect.start_x = static_cast<int>(det->mBox.mX * scale_x);
    rect.start_y = static_cast<int>(det->mBox.mY * scale_y);
    rect.crop_w = static_cast<int>(det->mBox.mWidth * scale_x);
    rect.crop_h = static_cast<int>(det->mBox.mHeight * scale_y);
    rects.push_back(rect);
  }
  if (!rects.empty()) {
    if (BM_SUCCESS != bmcv_image_draw_rectangle(
                          handle, frame, static_cast<int>(rects.size()),
                          rects.data(), thickness, bgr[2], bgr[1], bgr[0])) {
      IVS_WARN("CustomOsd bmcv draw rectangle failed");
    }
  }

  if (!mPutText) return;

  for (size_t i = 0; i < objectMetadata->mDetectedObjectMetadatas.size(); ++i) {
    if (filter_indices && filter_indices->count(i) == 0) continue;
    const auto& det = objectMetadata->mDetectedObjectMetadatas[i];
    int track_id = -1;
    if (i < objectMetadata->mTrackedObjectMetadatas.size()) {
      track_id = objectMetadata->mTrackedObjectMetadatas[i]->mTrackId;
    }

    std::string label;
    if (track_id >= 0) {
      label = "id:" + std::to_string(track_id);
    }
    if (!mClassNames.empty() && det->mClassify >= 0 &&
        det->mClassify < static_cast<int>(mClassNames.size())) {
      if (!label.empty()) label += " ";
      label += mClassNames[det->mClassify];
    }
    if (label.empty()) continue;

    int org_x = static_cast<int>(det->mBox.mX * scale_x);
    int org_y = static_cast<int>(det->mBox.mY * scale_y);
    if (org_y < static_cast<int>(20 * scale_y)) {
      org_y = static_cast<int>(20 * scale_y);
    }
    bmcv_point_t org = {org_x, org_y};
    bmcv_color_t color = {static_cast<unsigned char>(bgr[2]),
                          static_cast<unsigned char>(bgr[1]),
                          static_cast<unsigned char>(bgr[0])};
    if (BM_SUCCESS != bmcv_image_put_text(handle, frame, label.c_str(), org,
                                          color, font_scale, thickness)) {
      IVS_WARN("CustomOsd bmcv put text failed");
    }
  }
}

void CustomOsd::drawOverlaysOpenCv(bm_handle_t handle, const ChannelRule& rule,
                                   bm_image& frame) {
  cv::Mat mat;
  cv::bmcv::toMAT(&frame, mat);

  const int thickness = 2;
  for (const auto& roi : rule.rois) {
    if (roi.size() < 3) continue;
    std::vector<cv::Point> poly;
    poly.reserve(roi.size());
    for (const auto& pt : roi) {
      poly.emplace_back(pt.mX, pt.mY);
    }
    const cv::Point* pts = poly.data();
    int npts = static_cast<int>(poly.size());
    cv::polylines(mat, &pts, &npts, 1, true, cv::Scalar(0, 255, 0), thickness,
                  cv::LINE_AA);
  }

  for (const auto& line : rule.lines) {
    if (line.size() != 2) continue;
    cv::line(mat, cv::Point(line[0].mX, line[0].mY),
             cv::Point(line[1].mX, line[1].mY), cv::Scalar(0, 0, 255),
             thickness, cv::LINE_AA);
  }

  writeMatToBmImage(handle, mat, frame);
}

void CustomOsd::drawTrackBoxesOpenCv(
    bm_handle_t handle, std::shared_ptr<common::ObjectMetadata> objectMetadata,
    bm_image& frame) {
  cv::Mat mat;
  cv::bmcv::toMAT(&frame, mat);

  const int colors_num = static_cast<int>(kColors.size());
  const int thickness = 2;
  const float font_scale = 1.0f;

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

    const int box_x = det->mBox.mX;
    const int box_y = det->mBox.mY;
    const int box_w = det->mBox.mWidth;
    const int box_h = det->mBox.mHeight;
    cv::rectangle(mat, cv::Point(box_x, box_y),
                  cv::Point(box_x + box_w, box_y + box_h), color, thickness);

    if (!mPutText) continue;

    std::string label;
    if (track_id >= 0) {
      label = "id:" + std::to_string(track_id);
    }
    if (!mClassNames.empty() && det->mClassify >= 0 &&
        det->mClassify < static_cast<int>(mClassNames.size())) {
      if (!label.empty()) label += " ";
      label += mClassNames[det->mClassify];
    }
    if (label.empty()) continue;

    int org_x = det->mBox.mX;
    int org_y = det->mBox.mY;
    if (org_y < 20) org_y = 20;
    cv::putText(mat, label, cv::Point(org_x, org_y), cv::FONT_HERSHEY_SIMPLEX,
                font_scale, color, thickness);
  }

  writeMatToBmImage(handle, mat, frame);
}

void CustomOsd::draw(std::shared_ptr<common::ObjectMetadata> objectMetadata) {
  const int channel_id = objectMetadata->mFrame->mChannelId;
  ChannelRule rule;
  auto rule_it = mChannelRules.find(channel_id);
  if (rule_it != mChannelRules.end()) {
    rule = rule_it->second;
  }

  filterByRoi(objectMetadata, rule);

  bm_image src_image = objectMetadata->mFrame->mSpDataOsd
                           ? *(objectMetadata->mFrame->mSpDataOsd)
                           : *(objectMetadata->mFrame->mSpData);
  bm_handle_t handle = objectMetadata->mFrame->mHandle;
  const int src_w = src_image.width;
  const int src_h = src_image.height;

  bool need_save = false;
  std::unordered_set<size_t> crossing_indices =
      checkLineCrossing(objectMetadata, rule, need_save);

  // Fast skip: nothing visual to render — no ROIs, no lines, no detections
  // (boxes are always drawn for detections).  When the source is already in
  // the downstream format we can pass it through with zero buffer work.
  const bool has_draw_content = !rule.rois.empty() || !rule.lines.empty() ||
                                 !objectMetadata->mDetectedObjectMetadatas.empty();
  if (!has_draw_content && !need_save &&
      src_image.image_format == FORMAT_YUV420P) {
    std::shared_ptr<bm_image> output_image =
        objectMetadata->mFrame->mSpDataOsd
            ? objectMetadata->mFrame->mSpDataOsd
            : objectMetadata->mFrame->mSpData;
    objectMetadata->mFrame->mSpDataOsd = output_image;
    objectMetadata->mFrame->mSpData.reset();
    return;
  }

  // Determine drawing resolution and coordinate scaling.
  const int coord_w =
      objectMetadata->mFrame->mWidth > 0 ? objectMetadata->mFrame->mWidth : src_w;
  const int coord_h = objectMetadata->mFrame->mHeight > 0
                          ? objectMetadata->mFrame->mHeight
                          : src_h;
  const bool need_downscale =
      src_w > kOsdDrawMaxWidth || src_h > kOsdDrawMaxHeight;
  const int draw_w = need_downscale ? kOsdDrawMaxWidth : src_w;
  const int draw_h = need_downscale ? kOsdDrawMaxHeight : src_h;
  const float scale_x = static_cast<float>(draw_w) / coord_w;
  const float scale_y = static_cast<float>(draw_h) / coord_h;

  // Allocate draw buffer from pool (or create on first use).
  const bm_image_data_format_ext draw_dtype =
      need_downscale ? DATA_TYPE_EXT_1N_BYTE : src_image.data_type;
  bm_image draw_img_raw =
      allocateImage(handle, draw_w, draw_h, FORMAT_YUV420P, draw_dtype);

  if (need_downscale) {
    // VPP resize directly from source to draw resolution in one pass.
    bmcv_rect_t crop_rect{0, 0, src_w, src_h};
    bmcv_padding_atrr_t padding_attr;
    memset(&padding_attr, 0, sizeof(padding_attr));
    padding_attr.dst_crop_stx = 0;
    padding_attr.dst_crop_sty = 0;
    padding_attr.dst_crop_w = static_cast<unsigned int>(draw_w);
    padding_attr.dst_crop_h = static_cast<unsigned int>(draw_h);
    padding_attr.padding_b = 114;
    padding_attr.padding_g = 114;
    padding_attr.padding_r = 114;
    padding_attr.if_memset = 1;
    if (BM_SUCCESS != bmcv_image_vpp_convert_padding(
                          handle, 1, src_image, &draw_img_raw, &padding_attr,
                          &crop_rect)) {
      IVS_WARN("CustomOsd vpp resize failed: {}x{} -> {}x{}", src_w, src_h,
               draw_w, draw_h);
    }
  } else {
    bmcv_image_storage_convert(handle, 1, &src_image, &draw_img_raw);
  }

  // Wrap in shared_ptr with pool recycling as the deleter.
  std::shared_ptr<bm_image> draw_image(new bm_image(draw_img_raw),
                                       [this](bm_image* img) {
                                         recycleImage(*img);
                                         delete img;
                                       });

  // Switch draw backend here: Bmcv (device) or OpenCv (CPU).
  drawOverlaysBmcv(handle, rule, *draw_image, scale_x, scale_y);
  drawTrackBoxesBmcv(handle, objectMetadata, *draw_image, scale_x, scale_y);
  // drawOverlaysOpenCv(handle, rule, *draw_image);
  // drawTrackBoxesOpenCv(handle, objectMetadata, *draw_image);

  // Pass the draw buffer directly to downstream — do NOT upscale back
  // to the original resolution. Subsequent elements receive whatever size
  // we drew at (max 1080p).
  std::shared_ptr<bm_image> output_image = draw_image;

  if (need_save) {
    // Draw overlays + crossing-object boxes at original resolution for save.
    bm_image save_img = allocateImage(handle, src_w, src_h, FORMAT_YUV420P,
                                       src_image.data_type);
    bmcv_image_storage_convert(handle, 1, &src_image, &save_img);
    drawOverlaysBmcv(handle, rule, save_img, 1.0f, 1.0f);
    drawTrackBoxesBmcv(handle, objectMetadata, save_img, 1.0f, 1.0f,
                       &crossing_indices);
    cv::Mat save_frame;
    cv::bmcv::toMAT(&save_img, save_frame, true);
    saveCrossingImage(objectMetadata, save_frame);
    recycleImage(save_img);
  }

  objectMetadata->mFrame->mSpDataOsd = output_image;
  objectMetadata->mFrame->mSpData.reset();
}

bool CustomOsd::shouldDropFrame(int channel_id) {
  if (mDropInterval <= 1) return false;

  std::lock_guard<std::mutex> lk(mStateMtx);
  int& counter = mDropFrameCounters[channel_id];
  ++counter;
  return counter % mDropInterval != 0;
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
    if (shouldDropFrame(channel_id)) {
      return common::ErrorCode::SUCCESS;
    }

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
