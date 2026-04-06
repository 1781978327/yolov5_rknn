#ifndef RK_YOLO_H
#define TK_YOLO_H

#include "rknn_api.h"
#include "postprocess.h"
#include <string>
#include <vector>

class RkYolo
{
    typedef struct {
        int model_channel;
        int model_width;
        int model_height;
        std::string model_path;
    } Config_t;

public:
    RkYolo();
    ~RkYolo();

    int Init(rknn_core_mask mask);
    int Inference(int width, int height);
    void SetBuffers(uint8_t *inbuf, uint8_t *out_buf, int out_fd = -1)
    {
        m_inbuf = inbuf;
        m_outbuf = out_buf;
        m_outfd = out_fd;
    }

private:
    rknn_context m_rknn_ctx = 0;
    rknn_input_output_num m_io_num{};
    rknn_tensor_attr *m_input_attrs = nullptr;
    rknn_tensor_attr *m_output_attrs = nullptr;
    rknn_sdk_version m_version;
    rknn_input m_input;
    uint8_t *m_model_data = nullptr;
    rknn_tensor_mem* m_input_mem = nullptr;
    std::vector<rknn_tensor_mem*> m_output_mems;

    uint8_t *m_inbuf = nullptr;
    uint8_t *m_outbuf = nullptr;
    int m_outfd = -1;

    Config_t m_config;
    rknn_core_mask m_core_mask;

    int PreprocessToInputMem(int in_width, int in_height);
    int ConvertInputToOutputYuv(int in_width, int in_height);
    int DrawBoxesWithRga(const detect_result_group_t& group, int frame_width, int frame_height);
    void Destroy();
};

#endif
