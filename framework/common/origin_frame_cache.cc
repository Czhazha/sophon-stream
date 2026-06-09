//===----------------------------------------------------------------------===//
//
// Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
// SOPHON-STREAM is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//

#include "common/origin_frame_cache.h"

namespace sophon_stream {
namespace common {

OriginFrameCache& OriginFrameCache::getInstance() {
  static OriginFrameCache instance;
  return instance;
}

void OriginFrameCache::configure(size_t max_depth,
                                 std::int64_t max_frame_gap) {
  std::lock_guard<std::mutex> lk(mMtx);
  mMaxDepth = max_depth > 0 ? max_depth : 1;
  mMaxFrameGap = max_frame_gap > 0 ? max_frame_gap : 1;
}

void OriginFrameCache::put(int channel_id, std::int64_t frame_id,
                           std::int64_t timestamp_us, cv::Mat image) {
  if (image.empty()) return;

  std::lock_guard<std::mutex> lk(mMtx);
  auto& cache = mCaches[channel_id];
  cache.push_back({frame_id, timestamp_us, std::move(image)});
  while (cache.size() > mMaxDepth) {
    cache.pop_front();
  }
}

bool OriginFrameCache::getNearest(int channel_id,
                                  std::int64_t target_frame_id,
                                  CachedOriginFrame& out) const {
  std::lock_guard<std::mutex> lk(mMtx);
  auto it = mCaches.find(channel_id);
  if (it == mCaches.end() || it->second.empty()) {
    return false;
  }

  const CachedOriginFrame* exact = nullptr;
  const CachedOriginFrame* best_not_greater = nullptr;

  for (const auto& entry : it->second) {
    if (entry.frame_id == target_frame_id) {
      exact = &entry;
      break;
    }
    if (entry.frame_id <= target_frame_id) {
      if (!best_not_greater || entry.frame_id > best_not_greater->frame_id) {
        best_not_greater = &entry;
      }
    }
  }

  const CachedOriginFrame* chosen = exact ? exact : best_not_greater;
  if (!chosen) {
    return false;
  }

  if (!exact &&
      (target_frame_id - chosen->frame_id) > mMaxFrameGap) {
    return false;
  }

  out = *chosen;
  return true;
}

std::int64_t OriginFrameCache::getMaxFrameGap() const {
  std::lock_guard<std::mutex> lk(mMtx);
  return mMaxFrameGap;
}

}  // namespace common
}  // namespace sophon_stream
