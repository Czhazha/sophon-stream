// ===----------------------------------------------------------------------===
//
//  Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
//  SOPHON-DEMO is licensed under the 2-Clause BSD License except for the
//  third-party components.
//
// ===----------------------------------------------------------------------===

#ifndef SOPHON_STREAM_ELEMENT_QT_DISPLAY_BMLABEL_H_
#define SOPHON_STREAM_ELEMENT_QT_DISPLAY_BMLABEL_H_

#include <QApplication>
#include <QGridLayout>
#include <QImage>
#include <QLabel>
#include <QWidget>
#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>
#include "opencv2/opencv.hpp"

namespace sophon_stream {
namespace element {
namespace qt_display {

class BMLabel : public QLabel {
  Q_OBJECT
 public:
  explicit BMLabel(QWidget* parent, int width, int height);
  ~BMLabel();

  // Called from worker threads. Performs the heavy device->host copy and
  // pixel conversion here (NOT on the Qt GUI thread), then keeps only the
  // latest converted frame for display.
  void submit_frame(std::shared_ptr<bm_image> bmimg_ptr, float tmp_fps);

 public slots:
  // Runs on the Qt GUI thread: only does the lightweight setPixmap/update.
  void process_pending();

 private:
  QImage convert_frame(const std::shared_ptr<bm_image>& bmimg_ptr, float tmp_fps);

  QPixmap image_pixmap;
  std::mutex pending_mutex_;
  QImage pending_image_;
  std::atomic<bool> process_scheduled_{false};
};

}  // namespace qt_display
}  // namespace element
}  // namespace sophon_stream
#endif
