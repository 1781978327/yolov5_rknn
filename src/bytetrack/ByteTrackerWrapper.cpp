#include "ByteTrackerWrapper.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

ByteTrackerWrapper::ByteTrackerWrapper(int frame_rate, int track_buffer,
                                       float track_thresh, float high_thresh, float match_thresh)
    : tracker_(std::max(1, frame_rate), std::max(1, track_buffer),
               track_thresh, high_thresh, match_thresh)
    , frame_rate_(std::max(1, frame_rate))
{
}

detect_result_group_t ByteTrackerWrapper::Update(const detect_result_group_t& detections)
{
    std::vector<Object> objects;
    objects.reserve(detections.count);

    for (int i = 0; i < detections.count; ++i) {
        const detect_result_t& det = detections.results[i];
        const float x1 = static_cast<float>(det.box.left);
        const float y1 = static_cast<float>(det.box.top);
        const float x2 = static_cast<float>(det.box.right);
        const float y2 = static_cast<float>(det.box.bottom);
        if (x2 <= x1 || y2 <= y1) {
            continue;
        }

        Object obj;
        obj.rect = cv::Rect_<float>(x1, y1, x2 - x1, y2 - y1);
        obj.label = det.class_id;
        obj.prob = det.prop;
        obj.name = det.name;
        objects.push_back(std::move(obj));
    }

    std::vector<STrack> tracks = tracker_.update(objects, frame_rate_, ++frame_count_);

    detect_result_group_t tracked_group;
    memset(&tracked_group, 0, sizeof(tracked_group));
    tracked_group.id = detections.id;

    for (const auto& track : tracks) {
        if (tracked_group.count >= OBJ_NUMB_MAX_SIZE) {
            break;
        }

        detect_result_t& out = tracked_group.results[tracked_group.count++];
        memset(&out, 0, sizeof(out));
        std::snprintf(out.name, OBJ_NAME_MAX_SIZE, "%s", track.name.c_str());
        out.class_id = track.label;
        out.track_id = track.track_id;
        out.prop = track.score;
        out.box.left = static_cast<int>(std::lround(track.tlbr[0]));
        out.box.top = static_cast<int>(std::lround(track.tlbr[1]));
        out.box.right = static_cast<int>(std::lround(track.tlbr[2]));
        out.box.bottom = static_cast<int>(std::lround(track.tlbr[3]));

        const auto& trajectory = track.get_trajectory();
        const int total_points = static_cast<int>(trajectory.size());
        const int keep_points = std::min(total_points, TRAJECTORY_POINT_MAX_SIZE);
        out.trajectory_count = keep_points;
        const int start_index = std::max(0, total_points - keep_points);
        for (int i = 0; i < keep_points; ++i) {
            const auto& point = trajectory[start_index + i];
            out.trajectory[i].x = static_cast<int>(std::lround(point.first));
            out.trajectory[i].y = static_cast<int>(std::lround(point.second));
        }
    }

    return tracked_group;
}

void ByteTrackerWrapper::Reset()
{
    frame_count_ = 0;
    tracker_ = BYTETracker(frame_rate_);
}
