// ===----------------------------------------------------------------------===
//
//  Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
//  SOPHON-DEMO is licensed under the 2-Clause BSD License except for the
//  third-party components.
//
// ===----------------------------------------------------------------------===

#include "BMLabel.h"

#include <QMetaObject>
#include <QTimer>

#include <cstdio>
#include <algorithm>

namespace sophon_stream {
namespace element {
namespace qt_display {

BMLabel::BMLabel(QWidget* parent, int width, int height) : QLabel(parent) {
  setFixedSize(width, height);
  setContentsMargins(0, 0, 0, 0);
  setAlignment(Qt::AlignLeft | Qt::AlignTop);
}

BMLabel::~BMLabel() {}

void BMLabel::submit_frame(std::shared_ptr<bm_image> bmimg_ptr, float tmp_fps) {
  if (!bmimg_ptr) return;

  // Heavy work (device->host copy, resize, color convert) runs here on the
  // worker thread so the Qt GUI thread is not the throughput bottleneck.
  QImage image = convert_frame(bmimg_ptr, tmp_fps);
  if (image.isNull()) return;

  bool schedule = false;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_image_ = std::move(image);
    if (!process_scheduled_.exchange(true)) {
      schedule = true;
    }
  }

  if (schedule) {
    QTimer::singleShot(0, this, [this]() { process_pending(); });
  }
}

void BMLabel::process_pending() {
  QImage image;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    image = std::move(pending_image_);
    pending_image_ = QImage();
  }

  if (!image.isNull()) {
    image_pixmap = QPixmap::fromImage(image);
    setPixmap(image_pixmap);
    update();
  }

  bool schedule_again = false;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    if (!pending_image_.isNull()) {
      schedule_again = true;
    } else {
      process_scheduled_ = false;
    }
  }

  if (schedule_again) {
    QTimer::singleShot(0, this, [this]() { process_pending(); });
  }
}

QImage BMLabel::convert_frame(const std::shared_ptr<bm_image>& bmimg_ptr,
                              float tmp_fps) {
  int label_width = this->width();
  int label_height = this->height();

  cv::Mat mat_bgr;
  cv::bmcv::toMAT(bmimg_ptr.get(), mat_bgr, true);
  if (mat_bgr.empty()) return QImage();

  cv::Mat mat_resized;
  cv::resize(mat_bgr, mat_resized, cv::Size(label_width, label_height));

  char fps_text[64];
  std::snprintf(fps_text, sizeof(fps_text), "fps: %.1f", tmp_fps);
  const double font_scale = std::max(0.45, label_height / 540.0 * 0.55);
  const int thickness = std::max(1, static_cast<int>(font_scale * 2));
  int baseline = 0;
  cv::Size text_size = cv::getTextSize(fps_text, cv::FONT_HERSHEY_SIMPLEX,
                                       font_scale, thickness, &baseline);
  const int margin = std::max(4, label_height / 54);
  const int x = margin;
  const int y = label_height - margin;
  cv::rectangle(mat_resized,
                cv::Point(x - 2, y - text_size.height - baseline - 2),
                cv::Point(x + text_size.width + 2, y + baseline + 2),
                cv::Scalar(0, 0, 0), cv::FILLED);
  cv::putText(mat_resized, fps_text, cv::Point(x, y),
              cv::FONT_HERSHEY_SIMPLEX, font_scale, cv::Scalar(0, 255, 0),
              thickness);

  cv::Mat rgb_mat;
  if (mat_resized.channels() == 3) {
    cv::cvtColor(mat_resized, rgb_mat, cv::COLOR_BGR2RGB);
  } else if (mat_resized.channels() == 1) {
    cv::cvtColor(mat_resized, rgb_mat, cv::COLOR_GRAY2RGB);
  } else {
    rgb_mat = mat_resized;
  }

  // .copy() detaches from the local cv::Mat buffer so the QImage owns its data.
  return QImage((uchar*)rgb_mat.data, label_width, label_height, rgb_mat.step,
                QImage::Format_RGB888)
      .copy();
}

}  // namespace qt_display
}  // namespace element
}  // namespace sophon_stream
