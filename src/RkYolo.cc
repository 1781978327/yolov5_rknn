#include "RkYolo.h"
#include "INIReader.h"
#include "postprocess.h"
#include "log.h"
#include <rga.h>
#include <im2d.h>
#include <RgaUtils.h>
#include <string.h>
#include <algorithm>
#include <cstdint>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <opencv2/core/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <vector>

#if __has_include(<linux/dma-buf.h>)
#include <linux/dma-buf.h>
#else
#ifndef DMA_BUF_BASE
#define DMA_BUF_BASE 'b'
struct dma_buf_sync {
    uint64_t flags;
};
#define DMA_BUF_SYNC_READ        (1ULL << 0)
#define DMA_BUF_SYNC_WRITE       (2ULL << 0)
#define DMA_BUF_SYNC_RW          (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_START       (0ULL << 2)
#define DMA_BUF_SYNC_END         (1ULL << 2)
#define DMA_BUF_IOCTL_SYNC       _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)
#endif
#endif

static void drawTextWithBackground(cv::Mat &image, const std::string &text, cv::Point org,
                                   int fontFace, double fontScale, cv::Scalar textColor,
                                   cv::Scalar bgColor, int thickness);
static void fillPlaneRect(uint8_t* plane, int stride, int plane_width, int plane_height,
                          const cv::Rect& rect, uint8_t value);
static int dmaBufSync(int fd, bool start);
static detect_result_group_t BuildOverlayGroup(const detect_result_group_t& src,
                                               int frame_width, int frame_height);

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

int RkYolo::ConvertInputToOutputYuv(int in_width, int in_height)
{
    if (!m_inbuf || !m_outbuf) return -1;

    rga_buffer_t src = wrapbuffer_virtualaddr((void *)m_inbuf, in_width, in_height, RK_FORMAT_YUYV_422);
    rga_buffer_t dst;
    if (m_outfd > 0) {
        dst = wrapbuffer_fd(m_outfd, in_width, in_height, RK_FORMAT_YCbCr_420_P, in_width, in_height);
    } else {
        dst = wrapbuffer_virtualaddr((void *)m_outbuf, in_width, in_height, RK_FORMAT_YCbCr_420_P, in_width, in_height);
    }

    IM_STATUS status = imcvtcolor(src, dst, RK_FORMAT_YUYV_422, RK_FORMAT_YCbCr_420_P);
    if (status != IM_STATUS_SUCCESS && status != IM_STATUS_NOERROR) {
        LOG(ERROR, "%d, output yuv convert error! %s", __LINE__, imStrError(status));
        return -1;
    }
    return 0;
}

int RkYolo::DrawBoxesWithRga(const detect_result_group_t& group, int frame_width, int frame_height)
{
    if (!m_outbuf || frame_width <= 0 || frame_height <= 0) return -1;

    rga_buffer_t dst;
    if (m_outfd > 0) {
        dst = wrapbuffer_fd(m_outfd, frame_width, frame_height, RK_FORMAT_YCbCr_420_P, frame_width, frame_height);
    } else {
        dst = wrapbuffer_virtualaddr((void *)m_outbuf, frame_width, frame_height, RK_FORMAT_YCbCr_420_P, frame_width, frame_height);
    }

    // planar YUV + color fill 更适合走 RGA2，避免在 RK3588 上命中功能限制。
    (void)imconfig(IM_CONFIG_SCHEDULER_CORE, IM_SCHEDULER_RGA2_DEFAULT);

    const int thickness = 2;
    const int color = 0x00FF00;
    for (int i = 0; i < group.count; ++i) {
        const detect_result_t *det_result = &(group.results[i]);
        int x1 = det_result->box.left;
        int y1 = det_result->box.top;
        int x2 = det_result->box.right;
        int y2 = det_result->box.bottom;
        if (x2 <= x1 || y2 <= y1) continue;

        im_rect top = {x1, y1, x2 - x1, std::min(thickness, y2 - y1)};
        im_rect bottom = {x1, std::max(y1, y2 - thickness), x2 - x1, std::min(thickness, y2 - y1)};
        im_rect left = {x1, y1, std::min(thickness, x2 - x1), y2 - y1};
        im_rect right = {std::max(x1, x2 - thickness), y1, std::min(thickness, x2 - x1), y2 - y1};

        if (top.width > 0 && top.height > 0) (void)imfill(dst, top, color, IM_SYNC);
        if (bottom.width > 0 && bottom.height > 0) (void)imfill(dst, bottom, color, IM_SYNC);
        if (left.width > 0 && left.height > 0) (void)imfill(dst, left, color, IM_SYNC);
        if (right.width > 0 && right.height > 0) (void)imfill(dst, right, color, IM_SYNC);
    }
    return 0;
}

int RkYolo::DrawTextWithOpenCv(const detect_result_group_t& group, int frame_width, int frame_height)
{
    if (!m_outbuf || frame_width <= 0 || frame_height <= 0) return -1;
    if (group.count <= 0) return 0;
    if (m_outfd > 0) {
        (void)dmaBufSync(m_outfd, true);
    }

    const int y_stride = frame_width;
    const int uv_stride = frame_width / 2;
    uint8_t* y_ptr = m_outbuf;
    uint8_t* u_ptr = y_ptr + frame_width * frame_height;
    uint8_t* v_ptr = u_ptr + (frame_width / 2) * (frame_height / 2);

    cv::Mat y_plane(frame_height, frame_width, CV_8UC1, (void*)y_ptr, y_stride);
    cv::Mat u_plane(frame_height / 2, frame_width / 2, CV_8UC1, (void*)u_ptr, uv_stride);
    cv::Mat v_plane(frame_height / 2, frame_width / 2, CV_8UC1, (void*)v_ptr, uv_stride);

    char text[256];
    const double font_scale = 0.7;
    const int thickness = 2;
    const uint8_t text_y = 235;
    const uint8_t bg_y = 20;
    const uint8_t neutral_uv = 128;
    const int trajectory_thickness = 2;
    const int trajectory_uv_thickness = 1;
    const uint8_t trajectory_y = 150;
    const uint8_t trajectory_u = 44;
    const uint8_t trajectory_v = 21;
    for (int i = 0; i < group.count; i++) {
        const detect_result_t *det_result = &(group.results[i]);

        if (det_result->track_id >= 0 && det_result->trajectory_count > 1) {
            for (int j = 1; j < det_result->trajectory_count; ++j) {
                const cv::Point p0(det_result->trajectory[j - 1].x, det_result->trajectory[j - 1].y);
                const cv::Point p1(det_result->trajectory[j].x, det_result->trajectory[j].y);
                cv::line(y_plane, p0, p1, cv::Scalar(trajectory_y), trajectory_thickness, cv::LINE_AA);
                cv::line(u_plane, cv::Point(p0.x / 2, p0.y / 2), cv::Point(p1.x / 2, p1.y / 2),
                         cv::Scalar(trajectory_u), trajectory_uv_thickness, cv::LINE_AA);
                cv::line(v_plane, cv::Point(p0.x / 2, p0.y / 2), cv::Point(p1.x / 2, p1.y / 2),
                         cv::Scalar(trajectory_v), trajectory_uv_thickness, cv::LINE_AA);
            }

            const cv::Point last_point(det_result->trajectory[det_result->trajectory_count - 1].x,
                                       det_result->trajectory[det_result->trajectory_count - 1].y);
            cv::circle(y_plane, last_point, 3, cv::Scalar(trajectory_y), cv::FILLED, cv::LINE_AA);
            cv::circle(u_plane, cv::Point(last_point.x / 2, last_point.y / 2),
                       2, cv::Scalar(trajectory_u), cv::FILLED, cv::LINE_AA);
            cv::circle(v_plane, cv::Point(last_point.x / 2, last_point.y / 2),
                       2, cv::Scalar(trajectory_v), cv::FILLED, cv::LINE_AA);
        }

        if (det_result->track_id >= 0) {
            snprintf(text, sizeof(text), "ID:%d %s %.1f%%",
                     det_result->track_id, det_result->name, det_result->prop * 100);
        } else {
            snprintf(text, sizeof(text), "%s %.1f%%", det_result->name, det_result->prop * 100);
        }
        int baseline = 0;
        cv::Size text_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, font_scale, thickness, &baseline);
        baseline += thickness;

        int x = det_result->box.left;
        int y = std::max(text_size.height + baseline + 2, det_result->box.top - 6);
        if (x + text_size.width + 4 > frame_width) {
            x = std::max(0, frame_width - text_size.width - 4);
        }

        cv::Rect bg_rect(x,
                         std::max(0, y - text_size.height - baseline - 2),
                         std::min(frame_width - x, text_size.width + 4),
                         std::min(frame_height - std::max(0, y - text_size.height - baseline - 2),
                                  text_size.height + baseline + 4));
        if (bg_rect.width <= 0 || bg_rect.height <= 0) continue;

        cv::rectangle(y_plane, bg_rect, cv::Scalar(bg_y), cv::FILLED);

        cv::Rect uv_rect(bg_rect.x / 2,
                         bg_rect.y / 2,
                         (bg_rect.width + 1) / 2,
                         (bg_rect.height + 1) / 2);
        fillPlaneRect(u_ptr, uv_stride, frame_width / 2, frame_height / 2, uv_rect, neutral_uv);
        fillPlaneRect(v_ptr, uv_stride, frame_width / 2, frame_height / 2, uv_rect, neutral_uv);

        cv::putText(y_plane, text, cv::Point(x + 2, y - 2),
                    cv::FONT_HERSHEY_SIMPLEX, font_scale,
                    cv::Scalar(text_y), thickness, cv::LINE_8);
    }
    if (m_outfd > 0) {
        (void)dmaBufSync(m_outfd, false);
    }
    return 0;
}

int RkYolo::RenderOverlay(const detect_result_group_t& group, int frame_width, int frame_height)
{
    detect_result_group_t overlay_group = BuildOverlayGroup(group, frame_width, frame_height);

    // Keep OpenCV text as the last writer on the output buffer to avoid
    // hardware RGA fills partially covering or invalidating CPU-rendered labels.
    int ret = DrawBoxesWithRga(overlay_group, frame_width, frame_height);
    if (ret < 0) {
        LOG(ERROR, "draw boxes with rga failed");
        return ret;
    }

    ret = DrawTextWithOpenCv(overlay_group, frame_width, frame_height);
    if (ret < 0) {
        LOG(ERROR, "draw text with opencv failed");
        return ret;
    }

    return 0;
}

void drawTextWithBackground(cv::Mat &image, const std::string &text, cv::Point org, int fontFace, double fontScale, cv::Scalar textColor, cv::Scalar bgColor, int thickness)
{
    int baseline = 0;
    cv::Size textSize = cv::getTextSize(text, fontFace, fontScale, thickness, &baseline);
    baseline += thickness;
    cv::Rect textBgRect(org.x, org.y - textSize.height, textSize.width, textSize.height + baseline);
    cv::rectangle(image, textBgRect, bgColor, cv::FILLED);
    cv::putText(image, text, org, fontFace, fontScale, textColor, thickness, cv::LINE_AA);
}

static int dmaBufSync(int fd, bool start)
{
    if (fd < 0) return -1;
    struct dma_buf_sync sync;
    memset(&sync, 0, sizeof(sync));
    sync.flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | DMA_BUF_SYNC_RW;
    return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

static void fillPlaneRect(uint8_t* plane, int stride, int plane_width, int plane_height,
                          const cv::Rect& rect, uint8_t value)
{
    if (!plane || stride <= 0 || plane_width <= 0 || plane_height <= 0) return;

    int x0 = std::max(0, rect.x);
    int y0 = std::max(0, rect.y);
    int x1 = std::min(plane_width, rect.x + rect.width);
    int y1 = std::min(plane_height, rect.y + rect.height);
    if (x1 <= x0 || y1 <= y0) return;

    for (int y = y0; y < y1; ++y) {
        memset(plane + y * stride + x0, value, x1 - x0);
    }
}

static detect_result_group_t BuildOverlayGroup(const detect_result_group_t& src,
                                               int frame_width, int frame_height)
{
    detect_result_group_t dst;
    memset(&dst, 0, sizeof(dst));
    dst.id = src.id;

    for (int i = 0; i < src.count && dst.count < OBJ_NUMB_MAX_SIZE; ++i) {
        const detect_result_t* det = &src.results[i];
        int x1 = std::max(0, std::min(frame_width - 1, det->box.left));
        int y1 = std::max(0, std::min(frame_height - 1, det->box.top));
        int x2 = std::max(0, std::min(frame_width, det->box.right));
        int y2 = std::max(0, std::min(frame_height, det->box.bottom));
        if (x2 <= x1 || y2 <= y1) {
            continue;
        }

        detect_result_t* out = &dst.results[dst.count++];
        memcpy(out, det, sizeof(detect_result_t));
        out->box.left = x1;
        out->box.top = y1;
        out->box.right = x2;
        out->box.bottom = y2;
    }
    return dst;
}

int RkYolo::Inference(int in_width, int in_height)
{
    int ret = -1;
    memset(&m_last_detect_result_group, 0, sizeof(m_last_detect_result_group));
    ret = PreprocessToInputMem(in_width, in_height);
    if (ret < 0) {
        LOG(ERROR, "preprocess to rknn io_mem failed");
        return ret;
    }

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

    ret = ConvertInputToOutputYuv(in_width, in_height);
    if (ret < 0) {
        LOG(ERROR, "convert input to output yuv failed");
        return ret;
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
    ret = post_process((int8_t *)m_output_mems[0]->virt_addr,
                       (int8_t *)m_output_mems[1]->virt_addr,
                       (int8_t *)m_output_mems[2]->virt_addr,
                       m_config.model_height, m_config.model_width,
                       box_conf_threshold, nms_threshold, pads, scale_w, scale_h, out_zps, out_scales,
                       &detect_result_group);
    if (ret < 0) {
        LOG(ERROR, "post process failed ret=%d", ret);
        return ret;
    }

    m_last_detect_result_group = detect_result_group;

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
