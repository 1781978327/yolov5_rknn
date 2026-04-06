#ifndef TRANS_CODE_H
#define TRANS_CODE_H

#include <functional>
#include <atomic>
#include <future>
#include <deque>
#include <string>
#include <vector>

#include "MThread.h"
#include "V4l2Device.h"
#include "V4l2Capture.h"
#include "RkEncoder.h"
#include "RkYolo.h"
#include "ThreadPool.h"

class TransCoder : public MThread
{
public:
    typedef struct {
        int width;
        int height;
        int fps;
        int fix_qp;
        int rknn_thread;
        std::string device_name;
        std::string stream_name;
        std::string section_name;
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
        std::future<int> future;
    };

    Config_t config;
    bool ready_ = false;

    V4l2Capture *capture = nullptr;
    uint8_t *yuv_buf = nullptr;
    int yuv_size = 0;
    uint8_t *encodeData = nullptr;
    int frameSize = 0;
    std::function<void(std::vector<uint8_t> &&)> onEncodedDataCallback;

    RkEncoder *rk_encoder = nullptr;
    std::vector<RkYolo *> m_rkyolo_list;
    ThreadPool *m_pool = nullptr;
    int m_cur_yolo = 0;

    std::vector<std::vector<uint8_t>> m_in_buffer_list;
    std::vector<std::vector<uint8_t>> m_out_buffer_list;
    std::deque<PendingJob> pending_jobs_;
};

#endif
