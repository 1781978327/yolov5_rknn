#ifndef TRANS_CODE_H
#define TRANS_CODE_H

#include <functional>
#include <atomic>
#include <cstddef>
#include <future>
#include <deque>
#include <string>
#include <vector>

#include "MThread.h"
#include "ByteTrackerWrapper.h"
#include "RkEncoder.h"
#include "RkYolo.h"
#include "ThreadPool.h"

class TransCoder : public MThread
{
public:
    typedef struct {
        int fd = -1;
        void* va = nullptr;
        size_t size = 0;
    } DmaBuffer_t;

    typedef struct {
        int fd = -1;
        bool streaming = false;
        int width = 0;
        int height = 0;
        uint32_t pixfmt = 0;
        int buf_type = 0;
        bool mplane = false;
        int num_planes = 0;
        size_t plane0_size = 0;
        std::vector<DmaBuffer_t> buffers;
    } V4l2DmabufCapture_t;

    typedef struct {
        int width = 0;
        int height = 0;
        int fps = 0;
        int fix_qp = 0;
        int rknn_thread = 0;
        int dma_buffers = 0;
        bool tracker_enable = true;
        int track_buffer = 30;
        float track_thresh = 0.5f;
        float high_thresh = 0.6f;
        float match_thresh = 0.8f;
        std::string device_name;
        std::string stream_name;
        std::string section_name;
        std::string dma_heap;
    } Config_t;

    TransCoder();
    explicit TransCoder(const Config_t& cfg);
    ~TransCoder();
    void init();
    static std::vector<Config_t> LoadConfigs(const std::string& config_path);

    void run() override;
    void setOnEncoderDataCallback(std::function<void(std::vector<uint8_t> &&)> callback);

    TransCoder::Config_t const &getConfig() const;
    bool isReady() const;

private:
    struct PendingJob {
        int slot = -1;
        int input_size = 0;
        int capture_index = -1;
        RkYolo* yolo = nullptr;
        std::future<int> future;
    };

    Config_t config;
    bool ready_ = false;
    uint8_t *yuv_buf = nullptr;
    int yuv_size = 0;
    uint8_t *encodeData = nullptr;
    int frameSize = 0;
    std::function<void(std::vector<uint8_t> &&)> onEncodedDataCallback;

    RkEncoder *rk_encoder = nullptr;
    std::vector<RkYolo *> m_rkyolo_list;
    ByteTrackerWrapper *tracker_ = nullptr;
    ThreadPool *m_pool = nullptr;
    int m_cur_yolo = 0;
    int output_frame_size_ = 0;
    V4l2DmabufCapture_t dmabuf_capture_;

    std::vector<std::vector<uint8_t>> m_in_buffer_list;
    std::vector<DmaBuffer_t> output_dma_buffers_;
    std::deque<PendingJob> pending_jobs_;
};

#endif
