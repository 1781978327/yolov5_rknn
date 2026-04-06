#pragma once

#include <string>
#include <vector>

#include "deepsort.h"
#include "postprocess.h"

class DeepSortWrapper
{
public:
    DeepSortWrapper(const std::string& model_path,
                    int batch_size = 1,
                    int feature_dim = 512,
                    int reid_interval = 1,
                    int cpu_id = 6,
                    rknn_core_mask npu_id = RKNN_NPU_CORE_2);
    ~DeepSortWrapper();

    detect_result_group_t Update(const uint8_t* yuyv, int width, int height,
                                 const detect_result_group_t& detections);

private:
    void LoadLabels(const std::string& label_path);
    const char* LookupLabel(int class_id) const;

private:
    DeepSort tracker_;
    std::vector<std::string> labels_;
    int reid_interval_ = 1;
    long long frame_count_ = 0;
};
