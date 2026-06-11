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
#include <QPainter>
#include <QTimer>

#include <cstdio>
#include <algorithm>
#include <vector>

#include "common/common_defs.h"
#include "common/logger.h"
#include "opencv2/core/bmcv.hpp"

namespace sophon_stream {
namespace element {
namespace qt_display {

namespace {

QImage bm_image_rgb_packed_to_qimage(bm_image& image, int width, int height) {
  int byte_size = 0;
  if (bm_image_get_byte_size(image, &byte_size) != BM_SUCCESS || byte_size <= 0) {
    return QImage();
  }

  int stride[1] = {0};
  if (bm_image_get_stride(image, stride) != BM_SUCCESS || stride[0] <= 0) {
    return QImage();
  }

  std::vector<uint8_t> host_buf(static_cast<size_t>(byte_size));
  void* buffers[] = {host_buf.data()};
  if (bm_image_copy_device_to_host(image, buffers) != BM_SUCCESS) {
    return QImage();
  }

  return QImage(host_buf.data(), width, height, stride[0], QImage::Format_RGB888)
      .copy();
}

QImage mat_bgr_to_qimage(const cv::Mat& mat_bgr, int label_width,
                         int label_height) {
  cv::Mat rgb_mat;
  if (mat_bgr.channels() == 3) {
    cv::cvtColor(mat_bgr, rgb_mat, cv::COLOR_BGR2RGB);
  } else if (mat_bgr.channels() == 1) {
    cv::cvtColor(mat_bgr, rgb_mat, cv::COLOR_GRAY2RGB);
  } else {
    rgb_mat = mat_bgr;
  }

  return QImage((uchar*)rgb_mat.data, label_width, label_height, rgb_mat.step,
                QImage::Format_RGB888)
      .copy();
}

void draw_fps_osd_qimage(QImage& image, float tmp_fps, int label_height) {
  char fps_text[64];
  std::snprintf(fps_text, sizeof(fps_text), "fps: %.1f", tmp_fps);
  const double font_scale = std::max(0.45, label_height / 540.0 * 0.55);
  const int font_px = std::max(10, static_cast<int>(font_scale * 20));
  const int margin = std::max(4, label_height / 54);

  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing, false);
  QFont font = painter.font();
  font.setPixelSize(font_px);
  painter.setFont(font);

  const QFontMetrics fm(font);
  const int text_w = fm.width(fps_text);
  const int text_h = fm.height();
  const int x = margin;
  const int y = label_height - margin;

  painter.fillRect(x - 2, y - text_h, text_w + 4, text_h + 2, Qt::black);
  painter.setPen(Qt::green);
  painter.drawText(x, y - fm.descent(), fps_text);
}

void draw_fps_osd(cv::Mat& mat, float tmp_fps, int label_height) {
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
  cv::rectangle(mat,
                cv::Point(x - 2, y - text_size.height - baseline - 2),
                cv::Point(x + text_size.width + 2, y + baseline + 2),
                cv::Scalar(0, 0, 0), cv::FILLED);
  cv::putText(mat, fps_text, cv::Point(x, y), cv::FONT_HERSHEY_SIMPLEX,
              font_scale, cv::Scalar(0, 255, 0), thickness);
}

}  // namespace

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

QImage BMLabel::convert_frame_opencv(const std::shared_ptr<bm_image>& bmimg_ptr,
                                     float tmp_fps) {
  const int label_width = this->width();
  const int label_height = this->height();

  cv::Mat mat_bgr;
  cv::bmcv::toMAT(bmimg_ptr.get(), mat_bgr, true);
  if (mat_bgr.empty()) return QImage();

  cv::Mat mat_resized;
  cv::resize(mat_bgr, mat_resized, cv::Size(label_width, label_height));
  draw_fps_osd(mat_resized, tmp_fps, label_height);
  return mat_bgr_to_qimage(mat_resized, label_width, label_height);
}

QImage BMLabel::convert_frame(const std::shared_ptr<bm_image>& bmimg_ptr,
                              float tmp_fps) {
  const int label_width = this->width();
  const int label_height = this->height();

  bm_handle_t handle = bm_image_get_handle(bmimg_ptr.get());
  bm_image src_image = *bmimg_ptr;
  bm_image src_aligned;
  bool destroy_src_aligned = false;

  if (src_image.width & (64 - 1)) {
    int stride1[3], stride2[3];
    bm_image_get_stride(src_image, stride1);
    stride2[0] = FFALIGN(stride1[0], 64);
    stride2[1] = FFALIGN(stride1[1], 64);
    stride2[2] = FFALIGN(stride1[2], 64);
    bm_image_create(handle, src_image.height, src_image.width,
                    src_image.image_format, src_image.data_type, &src_aligned,
                    stride2);
    if (bm_image_alloc_dev_mem_heap_mask(src_aligned, STREAM_VPU_HEAP_MASK) !=
        BM_SUCCESS) {
      IVS_WARN("BMLabel convert_frame: alloc aligned src failed");
      return QImage();
    }
    bmcv_copy_to_atrr_t copy_attr;
    memset(&copy_attr, 0, sizeof(copy_attr));
    copy_attr.start_x = 0;
    copy_attr.start_y = 0;
    copy_attr.if_padding = 1;
    if (bmcv_image_copy_to(handle, copy_attr, src_image, src_aligned) !=
        BM_SUCCESS) {
      bm_image_destroy(src_aligned);
      IVS_WARN("BMLabel convert_frame: copy_to aligned src failed");
      return QImage();
    }
    src_image = src_aligned;
    destroy_src_aligned = true;
  }

  bm_image dst_rgb;
  bm_image_create(handle, label_height, label_width, FORMAT_RGB_PACKED,
                  bmimg_ptr->data_type, &dst_rgb);
  if (bm_image_alloc_dev_mem_heap_mask(dst_rgb, STREAM_VPU_HEAP_MASK) !=
      BM_SUCCESS) {
    if (destroy_src_aligned) bm_image_destroy(src_aligned);
    IVS_WARN("BMLabel convert_frame: alloc dst rgb failed");
    return QImage();
  }

  bmcv_rect_t crop_rect{0, 0, static_cast<unsigned int>(src_image.width),
                        static_cast<unsigned int>(src_image.height)};
  const bm_status_t vpp_ret =
      bmcv_image_vpp_convert(handle, 1, src_image, &dst_rgb, &crop_rect);
  if (destroy_src_aligned) bm_image_destroy(src_aligned);

  if (vpp_ret != BM_SUCCESS) {
    bm_image_destroy(dst_rgb);
    IVS_WARN("BMLabel convert_frame: vpp_convert failed, ret={0:d}",
             static_cast<int>(vpp_ret));
    return QImage();
  }

  QImage image = bm_image_rgb_packed_to_qimage(dst_rgb, label_width, label_height);
  bm_image_destroy(dst_rgb);
  if (image.isNull()) {
    IVS_WARN("BMLabel convert_frame: d2h qimage failed");
    return QImage();
  }

  draw_fps_osd_qimage(image, tmp_fps, label_height);
  return image;
}

}  // namespace qt_display
}  // namespace element
}  // namespace sophon_stream

#if 0
// Legacy entry kept for quick A/B switch during perf comparison:
// return convert_frame_opencv(bmimg_ptr, tmp_fps);
#endif
