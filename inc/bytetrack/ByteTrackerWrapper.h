#pragma once

#include "BYTETracker.h"
#include "postprocess.h"

class ByteTrackerWrapper
{
public:
    ByteTrackerWrapper(int frame_rate = 30, int track_buffer = 30,
                       float track_thresh = 0.5f, float high_thresh = 0.6f, float match_thresh = 0.8f);

    detect_result_group_t Update(const detect_result_group_t& detections);
    void Reset();

private:
    BYTETracker tracker_;
    long long frame_count_ = 0;
    int frame_rate_ = 30;
};
