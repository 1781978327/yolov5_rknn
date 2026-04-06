#include "DeepSortWrapper.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include <opencv2/imgproc.hpp>

namespace {
const char* kDefaultLabelPath = "./model/coco_80_labels_list.txt";
}

DeepSortWrapper::DeepSortWrapper(const std::string& model_path,
                                 int batch_size,
                                 int feature_dim,
                                 int reid_interval,
                                 int cpu_id,
                                 rknn_core_mask npu_id)
    : tracker_(model_path, batch_size, feature_dim, cpu_id, npu_id)
    , reid_interval_(std::max(1, reid_interval))
{
    LoadLabels(kDefaultLabelPath);
}

DeepSortWrapper::~DeepSortWrapper()
{
}

detect_result_group_t DeepSortWrapper::Update(const uint8_t* yuyv, int width, int height,
                                              const detect_result_group_t& detections)
{
    detect_result_group_t tracked_group;
    memset(&tracked_group, 0, sizeof(tracked_group));
    tracked_group.id = detections.id;

    if (!yuyv || width <= 0 || height <= 0) {
        return tracked_group;
    }

    frame_count_++;
    const bool run_reid =
        detections.count > 0 &&
        (reid_interval_ <= 1 || ((frame_count_ - 1) % reid_interval_ == 0));

    std::vector<DetectBox> dets;
    if (run_reid) {
        cv::Mat yuyv_mat(height, width, CV_8UC2, const_cast<uint8_t*>(yuyv));
        cv::Mat bgr_mat;
        cv::cvtColor(yuyv_mat, bgr_mat, cv::COLOR_YUV2BGR_YUYV);

        dets.reserve(detections.count);
        for (int i = 0; i < detections.count; ++i) {
            const detect_result_t& det = detections.results[i];
            if (det.box.right <= det.box.left || det.box.bottom <= det.box.top) {
                continue;
            }

            DetectBox box{};
            box.x1 = static_cast<float>(det.box.left);
            box.y1 = static_cast<float>(det.box.top);
            box.x2 = static_cast<float>(det.box.right);
            box.y2 = static_cast<float>(det.box.bottom);
            box.confidence = det.prop;
            box.classID = static_cast<float>(det.class_id);
            dets.push_back(box);
        }

        if (!dets.empty()) {
            tracker_.sort(bgr_mat, dets);
        } else {
            tracker_.sort_interval(bgr_mat, dets);
        }
    } else {
        cv::Mat dummy;
        tracker_.sort_interval(dummy, dets);
    }
    std::vector<Track> tracks = tracker_.get_confirmed_tracks();

    for (const auto& track : tracks) {
        if (tracked_group.count >= OBJ_NUMB_MAX_SIZE) {
            break;
        }
        if (track.time_since_update > std::max(1, reid_interval_)) {
            continue;
        }

        DETECTBOX tlwh = track.to_tlwh();
        detect_result_t& out = tracked_group.results[tracked_group.count++];
        memset(&out, 0, sizeof(out));

        out.class_id = track.cls;
        out.track_id = track.track_id;
        out.prop = track.conf >= 0.0f ? track.conf : 1.0f;
        out.box.left = static_cast<int>(std::lround(tlwh(0)));
        out.box.top = static_cast<int>(std::lround(tlwh(1)));
        out.box.right = static_cast<int>(std::lround(tlwh(0) + tlwh(2)));
        out.box.bottom = static_cast<int>(std::lround(tlwh(1) + tlwh(3)));
        std::snprintf(out.name, OBJ_NAME_MAX_SIZE, "%s", LookupLabel(out.class_id));

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

    // DeepSort only exposes confirmed tracks. Before confirmation, or when
    // association temporarily fails, fall back to the raw detector output so
    // the stream still keeps visible boxes/text.
    if (tracked_group.count == 0 && detections.count > 0) {
        tracked_group = detections;
    }

    return tracked_group;
}

void DeepSortWrapper::LoadLabels(const std::string& label_path)
{
    labels_.clear();

    std::ifstream file(label_path);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        labels_.push_back(line);
    }
}

const char* DeepSortWrapper::LookupLabel(int class_id) const
{
    static const char* kUnknown = "unknown";
    if (class_id < 0 || class_id >= static_cast<int>(labels_.size())) {
        return kUnknown;
    }
    return labels_[class_id].c_str();
}
