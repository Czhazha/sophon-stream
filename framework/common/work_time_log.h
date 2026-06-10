//===----------------------------------------------------------------------===//
//
// Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
// SOPHON-STREAM is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//

#ifndef SOPHON_STREAM_COMMON_WORK_TIME_LOG_H_
#define SOPHON_STREAM_COMMON_WORK_TIME_LOG_H_

#include <cstdio>
#include <mutex>
#include <string>
#include <sys/time.h>
#include <unordered_map>

namespace sophon_stream {
namespace common {

inline std::string formatTimeOfDayMs() {
  timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm tm_local;
  localtime_r(&tv.tv_sec, &tm_local);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", tm_local.tm_hour,
                tm_local.tm_min, tm_local.tm_sec,
                static_cast<int>(tv.tv_usec / 1000));
  return std::string(buf);
}

class WorkTimeLogGate {
 public:
  explicit WorkTimeLogGate(int interval = 100) : mInterval(interval) {}

  bool tick(int channel_id) {
    std::lock_guard<std::mutex> lk(mMtx);
    int& count = mCounters[channel_id];
    ++count;
    if (count < mInterval) {
      return false;
    }
    count = 0;
    return true;
  }

 private:
  int mInterval;
  std::mutex mMtx;
  std::unordered_map<int, int> mCounters;
};

}  // namespace common
}  // namespace sophon_stream

#endif  // SOPHON_STREAM_COMMON_WORK_TIME_LOG_H_
