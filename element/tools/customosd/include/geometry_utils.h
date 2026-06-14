//===----------------------------------------------------------------------===//
//
// Copyright (C) 2022 Sophgo Technologies Inc.  All rights reserved.
//
// SOPHON-STREAM is licensed under the 2-Clause BSD License except for the
// third-party components.
//
//===----------------------------------------------------------------------===//

#ifndef SOPHON_STREAM_ELEMENT_CUSTOMOSD_GEOMETRY_UTILS_H_
#define SOPHON_STREAM_ELEMENT_CUSTOMOSD_GEOMETRY_UTILS_H_

#include <limits>
#include <vector>

#include "common/graphics.h"

namespace sophon_stream {
namespace element {
namespace customosd {

inline bool onSegment(const common::Point<int>& p, const common::Point<int>& q,
                      const common::Point<int>& r) {
  return q.mX <= std::max(p.mX, r.mX) && q.mX >= std::min(p.mX, r.mX) &&
         q.mY <= std::max(p.mY, r.mY) && q.mY >= std::min(p.mY, r.mY);
}

inline int orientation(const common::Point<int>& p, const common::Point<int>& q,
                       const common::Point<int>& r) {
  double val =
      1.0 * (q.mY - p.mY) * (r.mX - q.mX) - 1.0 * (q.mX - p.mX) * (r.mY - q.mY);
  if (val == 0) return 0;
  return (val > 0) ? 1 : 2;
}

inline bool doIntersect(const common::Point<int>& p1, const common::Point<int>& q1,
                        const common::Point<int>& p2, const common::Point<int>& q2) {
  int o1 = orientation(p1, q1, p2);
  int o2 = orientation(p1, q1, q2);
  int o3 = orientation(p2, q2, p1);
  int o4 = orientation(p2, q2, q1);

  if (o1 != o2 && o3 != o4) return true;

  if (o1 == 0 && onSegment(p1, p2, q1)) return true;
  if (o2 == 0 && onSegment(p1, q2, q1)) return true;
  if (o3 == 0 && onSegment(p2, p1, q2)) return true;
  if (o4 == 0 && onSegment(p2, q1, q2)) return true;

  return false;
}

inline bool isPointInsidePolygon(
    const common::Point<int>& p,
    const std::vector<common::Point<int>>& polygon) {
  int n = polygon.size();
  if (n < 3) return false;

  common::Point<int> extreme = {std::numeric_limits<int>::max(), p.mY};
  int count = 0;
  int i = 0;
  do {
    int next = (i + 1) % n;
    if (doIntersect(polygon[i], polygon[next], p, extreme)) {
      if (orientation(polygon[i], p, polygon[next]) == 0)
        return onSegment(polygon[i], p, polygon[next]);
      count++;
    }
    i = next;
  } while (i != 0);

  return count & 1;
}

inline bool isPointInAnyRoi(
    const common::Point<int>& p,
    const std::vector<std::vector<common::Point<int>>>& rois) {
  if (rois.empty()) return true;
  for (const auto& roi : rois) {
    if (roi.size() >= 3 && isPointInsidePolygon(p, roi)) return true;
  }
  return false;
}

inline bool isSegmentCrossingLine(
    const common::Point<int>& prev_center, const common::Point<int>& curr_center,
    const std::vector<common::Point<int>>& line) {
  if (line.size() != 2) return false;
  return doIntersect(prev_center, curr_center, line[0], line[1]);
}

// Check if a rectangle intersects (touches or crosses) a line segment.
// Returns true when:
//   - either endpoint of the line lies on/inside the rectangle, OR
//   - the line segment crosses any of the rectangle's four edges.
inline bool isRectIntersectingLine(
    const common::Rectangle<int>& rect,
    const std::vector<common::Point<int>>& line) {
  if (line.size() != 2) return false;

  const int l = rect.left(), r = rect.right();
  const int t = rect.top(), b = rect.bottom();

  // Either line endpoint inside (or on the boundary of) the rectangle?
  if ((line[0].mX >= l && line[0].mX <= r &&
       line[0].mY >= t && line[0].mY <= b) ||
      (line[1].mX >= l && line[1].mX <= r &&
       line[1].mY >= t && line[1].mY <= b)) {
    return true;
  }

  // Check line vs the four edges of the rectangle.
  const common::Point<int> tl(l, t), tr(r, t), bl(l, b), br(r, b);
  if (doIntersect(line[0], line[1], tl, tr)) return true;  // top
  if (doIntersect(line[0], line[1], bl, br)) return true;  // bottom
  if (doIntersect(line[0], line[1], tl, bl)) return true;  // left
  if (doIntersect(line[0], line[1], tr, br)) return true;  // right

  return false;
}

}  // namespace customosd
}  // namespace element
}  // namespace sophon_stream

#endif  // SOPHON_STREAM_ELEMENT_CUSTOMOSD_GEOMETRY_UTILS_H_
