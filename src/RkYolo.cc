#include "RkYolo.h"
#include "INIReader.h"
#include "postprocess.h"
#include "log.h"
#include <rga.h>
#include <im2d.h>
#include <RgaUtils.h>
#include <string.h>
#include <opencv2/core/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <vector>

RkYolo::RkYolo()
{
}

RkYolo::~RkYolo()
{
    Destroy();
}

int RkYolo::Init(rknn_core_mask mask)
{
    int ret = -1;
    FILE *fp;

    INIReader ini("configs/config.ini");
    m_config.model_path = ini.Get("rknn", "model_path", "model/yolov5.rknn");

    fp = fopen(m_config.model_path.c_str(), "rb");
    if (!fp) {
        LOG(ERROR, "open model failed: %s", m_config.model_path.c_str());
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    int size = ftell(fp);

    m_model_data = (uint8_t *)malloc(size);
    if (m_model_data == nullptr) {
        return -1;
    }

    ret = fseek(fp, 0, SEEK_SET);
    if (ret < 0) {
        LOG(ERROR, "blob seek failure.");
        return -1;
    }

    memset(m_model_data, 0, sizeof(m_model_data));
    ret = fread(m_model_data, 1, size, fp);
    if (ret < 0) {
        LOG(ERROR, "laod model failed");
        fclose(fp);
        return -1;
    }
    fclose(fp);

    ret = rknn_init(&m_rknn_ctx, m_model_data, size, 0, NULL);
    if (ret < 0) {
        LOG(ERROR, "rknn_init error ret=%d", ret);
        return -1;
    }

    m_core_mask = mask;
    ret = rknn_set_core_mask(m_rknn_ctx, m_core_mask);
    if (ret < 0) {
        LOG(ERROR, "rknn_init core error ret=%d\n", ret);
        return -1;
    }

    // get rknn version
    ret = rknn_query(m_rknn_ctx, RKNN_QUERY_SDK_VERSION, &m_version, sizeof(rknn_sdk_version));
    if (ret < 0) {
        LOG(ERROR, "rknn query version failed.");
        return -1;
    }

    // get rknn params
    ret = rknn_query(m_rknn_ctx, RKNN_QUERY_IN_OUT_NUM, &m_io_num, sizeof(m_io_num));
    if (ret < 0) {
        LOG(ERROR, "rknn query io_num failed.");
        return -1;
    }

    // set rknn input arr
    m_input_attrs = new rknn_tensor_attr[m_io_num.n_input];
    memset(m_input_attrs, 0, sizeof(rknn_tensor_attr) * m_io_num.n_input);
    for (int i = 0; i < m_io_num.n_input; i++) {
        m_input_attrs[i].index = i;
        ret = rknn_query(m_rknn_ctx, RKNN_QUERY_INPUT_ATTR, &(m_input_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret < 0) {
            LOG(ERROR, "rknn_init error ret=%d", ret);
            return -1;
        }
    }

    // set rknn output arr
    m_output_attrs = new rknn_tensor_attr[m_io_num.n_output];
    memset(m_output_attrs, 0, sizeof(rknn_tensor_attr) * m_io_num.n_output);
    for (int i = 0; i < m_io_num.n_output; i++) {
        m_output_attrs[i].index = i;
        ret = rknn_query(m_rknn_ctx, RKNN_QUERY_OUTPUT_ATTR, &(m_output_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret < 0) {
            LOG(ERROR, "rknn query output attr failed ret=%d", ret);
            return -1;
        }
    }

    // set rknn input params
    if (m_input_attrs[0].fmt == RKNN_TENSOR_NCHW) {
        LOG(NOTICE, "model is NCHW input fmt npu_core:%d", m_core_mask >> 1);
        m_config.model_channel = m_input_attrs[0].dims[1];
        m_config.model_height = m_input_attrs[0].dims[2];
        m_config.model_width = m_input_attrs[0].dims[3];
    } else {
        LOG(NOTICE, "model is NHWC input fmt npu_core:%d", m_core_mask >> 1);
        m_config.model_height = m_input_attrs[0].dims[1];
        m_config.model_width = m_input_attrs[0].dims[2];
        m_config.model_channel = m_input_attrs[0].dims[3];
    }

    m_input.index = 0;
    m_input.type = RKNN_TENSOR_UINT8;
    m_input.size = m_config.model_width * m_config.model_height * m_config.model_channel;
    m_input.fmt = RKNN_TENSOR_NHWC;
    m_input.pass_through = 0;

    m_input_attrs[0].type = RKNN_TENSOR_UINT8;
    m_input_attrs[0].fmt = RKNN_TENSOR_NHWC;
    m_input_attrs[0].pass_through = 0;
    if (m_input_attrs[0].w_stride == 0) m_input_attrs[0].w_stride = m_config.model_width;
    if (m_input_attrs[0].h_stride == 0) m_input_attrs[0].h_stride = m_config.model_height;

    m_input_mem = rknn_create_mem(m_rknn_ctx, m_input_attrs[0].size_with_stride);
    if (!m_input_mem) {
        LOG(ERROR, "rknn_create_mem input failed");
        return -1;
    }
    ret = rknn_set_io_mem(m_rknn_ctx, m_input_mem, &m_input_attrs[0]);
    if (ret < 0) {
        LOG(ERROR, "rknn_set_io_mem input failed ret=%d", ret);
        return -1;
    }

    m_output_mems.resize(m_io_num.n_output, nullptr);
    for (uint32_t i = 0; i < m_io_num.n_output; ++i) {
        m_output_mems[i] = rknn_create_mem(m_rknn_ctx, m_output_attrs[i].size_with_stride);
        if (!m_output_mems[i]) {
            LOG(ERROR, "rknn_create_mem output failed idx=%u", i);
            return -1;
        }
        ret = rknn_set_io_mem(m_rknn_ctx, m_output_mems[i], &m_output_attrs[i]);
        if (ret < 0) {
            LOG(ERROR, "rknn_set_io_mem output failed idx=%u ret=%d", i, ret);
            return -1;
        }
    }

    return ret;
}

int RkYolo::PreprocessToInputMem(int in_width, int in_height)
{
    if (!m_inbuf || !m_input_mem) return -1;

    const int dst_w = m_config.model_width;
    const int dst_h = m_config.model_height;
    const int dst_w_stride = m_input_attrs[0].w_stride > 0 ? (int)m_input_attrs[0].w_stride : dst_w;
    const int dst_h_stride = m_input_attrs[0].h_stride > 0 ? (int)m_input_attrs[0].h_stride : dst_h;

    rga_buffer_t src = wrapbuffer_virtualaddr((void *)m_inbuf, in_width, in_height, RK_FORMAT_YUYV_422);
    rga_buffer_t dst;
    if (m_input_mem->fd > 0) {
        dst = wrapbuffer_fd(m_input_mem->fd, dst_w, dst_h, RK_FORMAT_RGB_888, dst_w_stride, dst_h_stride);
    } else {
        dst = wrapbuffer_virtualaddr(m_input_mem->virt_addr, dst_w, dst_h, RK_FORMAT_RGB_888, dst_w_stride, dst_h_stride);
    }

    im_rect src_rect = {0, 0, in_width, in_height};
    im_rect dst_rect = {0, 0, dst_w, dst_h};
    int check = imcheck(src, dst, src_rect, dst_rect);
    if (check != IM_STATUS_SUCCESS && check != IM_STATUS_NOERROR) {
        LOG(ERROR, "%d, rga check error! %s", __LINE__, imStrError((IM_STATUS)check));
        return -1;
    }

    IM_STATUS status = improcess(src, dst,
                                 wrapbuffer_virtualaddr(nullptr, 0, 0, RK_FORMAT_RGBA_8888),
                                 src_rect, dst_rect, im_rect{0, 0, 0, 0}, IM_SYNC);
    if (status != IM_STATUS_SUCCESS && status != IM_STATUS_NOERROR) {
        LOG(ERROR, "%d, improcess error! %s", __LINE__, imStrError(status));
        return -1;
    }

    return rknn_mem_sync(m_rknn_ctx, m_input_mem, RKNN_MEMORY_SYNC_TO_DEVICE);
}

void drawTextWithBackground(cv::Mat &image, const std::string &text, cv::Point org, int fontFace, double fontScale, cv::Scalar textColor, cv::Scalar bgColor, int thickness)
{
    int baseline = 0;
    cv::Size textSize = cv::getTextSize(text, fontFace, fontScale, thickness, &baseline);
    baseline += thickness;
    cv::Rect textBgRect(org.x, org.y - textSize.height, textSize.width, textSize.height + baseline);
    cv::rectangle(image, textBgRect, bgColor, cv::FILLED);
    cv::putText(image, text, org, fontFace, fontScale, textColor, thickness);
}

int RkYolo::Inference(int in_width, int in_height)
{
    int ret = -1;
    ret = PreprocessToInputMem(in_width, in_height);
    if (ret < 0) {
        LOG(ERROR, "preprocess to rknn io_mem failed");
        return ret;
    }

    cv::Mat yuvImage(in_height, in_width, CV_8UC2, (void *)m_inbuf);
    cv::Mat ori_img;
    cvtColor(yuvImage, ori_img, cv::COLOR_YUV2RGB_YUY2); // Use COLOR_YUV2BGR_YUY2 for YUYV format

    ret = rknn_run(m_rknn_ctx, NULL);
    if (ret < 0) {
        LOG(ERROR, "rknn_run failed ret=%d", ret);
        return ret;
    }
    for (auto* mem : m_output_mems) {
        if (mem) {
            rknn_mem_sync(m_rknn_ctx, mem, RKNN_MEMORY_SYNC_FROM_DEVICE);
        }
    }

    // post process
    const float nms_threshold = NMS_THRESH;
    const float box_conf_threshold = BOX_THRESH;
    BOX_RECT pads;
    memset(&pads, 0, sizeof(BOX_RECT));
    float scale_w = (float)m_config.model_width / in_width;
    float scale_h = (float)m_config.model_height / in_height;

    detect_result_group_t detect_result_group;
    std::vector<float> out_scales;
    std::vector<int32_t> out_zps;
    for (int i = 0; i < m_io_num.n_output; ++i) {
        out_scales.push_back(m_output_attrs[i].scale);
        out_zps.push_back(m_output_attrs[i].zp);
    }
    post_process((int8_t *)m_output_mems[0]->virt_addr, (int8_t *)m_output_mems[1]->virt_addr, (int8_t *)m_output_mems[2]->virt_addr,
                 m_config.model_height, m_config.model_width,
                 box_conf_threshold, nms_threshold, pads, scale_w, scale_h, out_zps, out_scales, &detect_result_group);

    // Draw Objects
    char text[256];
    for (int i = 0; i < detect_result_group.count; i++) {
        detect_result_t *det_result = &(detect_result_group.results[i]);
        sprintf(text, "%s %.1f%%", det_result->name, det_result->prop * 100);
        int x1 = det_result->box.left;
        int y1 = det_result->box.top;
        int x2 = det_result->box.right;
        int y2 = det_result->box.bottom;
        cv::Rect rect(x1, y1, x2 - x1, y2 - y1);
        cv::Scalar color = cv::Scalar(0, 55, 218);
        cv::rectangle(ori_img, rect, color, 2);
        drawTextWithBackground(ori_img, text, cv::Point(x1 - 1, y1 - 6), cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(255, 255, 255), cv::Scalar(0, 55, 218, 0.5), 2);
    }

    cv::Mat resized_img;
    resize(ori_img, resized_img, cv::Size(in_width, in_height));

    cv::Mat yuv_img;
    cv::cvtColor(resized_img, yuv_img, cv::COLOR_RGB2YUV_I420);

    memcpy(m_outbuf, yuv_img.data, in_width * in_height * 3 / 2);

    return ret;
}

void RkYolo::Destroy()
{
    if (m_rknn_ctx && m_input_mem) {
        rknn_destroy_mem(m_rknn_ctx, m_input_mem);
        m_input_mem = nullptr;
    }
    for (auto*& mem : m_output_mems) {
        if (m_rknn_ctx && mem) {
            rknn_destroy_mem(m_rknn_ctx, mem);
        }
        mem = nullptr;
    }
    m_output_mems.clear();
    if (m_rknn_ctx) {
        rknn_destroy(m_rknn_ctx);
        m_rknn_ctx = 0;
    }
    delete[] m_input_attrs;
    m_input_attrs = nullptr;
    delete[] m_output_attrs;
    m_output_attrs = nullptr;
    if (m_model_data)
        free(m_model_data);
    m_model_data = nullptr;
}
