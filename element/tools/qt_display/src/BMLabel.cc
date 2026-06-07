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

namespace sophon_stream {
namespace element {
namespace qt_display {

BMLabel::BMLabel(QWidget* parent, int width, int height) : QLabel(parent) {
  setFixedSize(width, height);
}

BMLabel::~BMLabel() {}

void BMLabel::submit_frame(std::shared_ptr<bm_image> bmimg_ptr) {
  if (!bmimg_ptr) return;

  bool schedule = false;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_img_ = std::move(bmimg_ptr);
    if (!process_scheduled_.exchange(true)) {
      schedule = true;
    }
  }

  if (schedule) {
    QTimer::singleShot(0, this, [this]() { process_pending(); });
  }
}

void BMLabel::process_pending() {
  std::shared_ptr<bm_image> img;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    img = pending_img_;
    pending_img_.reset();
  }

  if (img) {
    render_frame(img);
  }

  bool schedule_again = false;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    if (pending_img_) {
      schedule_again = true;
    } else {
      process_scheduled_ = false;
    }
  }

  if (schedule_again) {
    QTimer::singleShot(0, this, [this]() { process_pending(); });
  }
}

void BMLabel::render_frame(const std::shared_ptr<bm_image>& bmimg_ptr) {
  int label_width = this->width();
  int label_height = this->height();

  cv::Mat mat_bgr;
  cv::bmcv::toMAT(bmimg_ptr.get(), mat_bgr, true);
  if (mat_bgr.empty()) return;

  cv::Mat mat_resized;
  cv::resize(mat_bgr, mat_resized, cv::Size(label_width, label_height));

  cv::Mat rgb_mat;
  if (mat_resized.channels() == 3) {
    cv::cvtColor(mat_resized, rgb_mat, cv::COLOR_BGR2RGB);
  } else if (mat_resized.channels() == 1) {
    cv::cvtColor(mat_resized, rgb_mat, cv::COLOR_GRAY2RGB);
  } else {
    rgb_mat = mat_resized;
  }

  QImage image((uchar*)rgb_mat.data, label_width, label_height, rgb_mat.step,
               QImage::Format_RGB888);
  image_pixmap = QPixmap::fromImage(image.copy());
  setPixmap(image_pixmap);
  update();
}

}  // namespace qt_display
}  // namespace element
}  // namespace sophon_stream
