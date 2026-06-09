//===----------------------------------------------------------------------===//
//
// Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
// SOPHON-STREAM is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//

#ifndef SOPHON_STREAM_COMMON_ORIGIN_FRAME_CACHE_H_
#define SOPHON_STREAM_COMMON_ORIGIN_FRAME_CACHE_H_

#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>

#include <opencv2/opencv.hpp>

namespace sophon_stream {
namespace common {

struct CachedOriginFrame {
  std::int64_t frame_id = -1;
  std::int64_t timestamp_us = 0;
  cv::Mat image;
};

class OriginFrameCache {
 public:
  static OriginFrameCache& getInstance();

  void configure(size_t max_depth, std::int64_t max_frame_gap);
  void put(int channel_id, std::int64_t frame_id, std::int64_t timestamp_us,
           cv::Mat image);
  bool getNearest(int channel_id, std::int64_t target_frame_id,
                  CachedOriginFrame& out) const;
  std::int64_t getMaxFrameGap() const;

 private:
  OriginFrameCache() = default;

  mutable std::mutex mMtx;
  std::unordered_map<int, std::deque<CachedOriginFrame>> mCaches;
  size_t mMaxDepth = 3;
  std::int64_t mMaxFrameGap = 10;
};

}  // namespace common
}  // namespace sophon_stream

#endif  // SOPHON_STREAM_COMMON_ORIGIN_FRAME_CACHE_H_
