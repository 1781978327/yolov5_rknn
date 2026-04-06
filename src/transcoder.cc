#include <list>
#include <string.h>
#include <vector>
#include <functional>
#include <sstream>
#include "transcoder.h"
#include "INIReader.h"
#include "log.h"

TransCoder::TransCoder()
{
    std::vector<Config_t> configs = LoadConfigs("./configs/config.ini");
    if (!configs.empty()) {
        config = configs.front();
    }
    init();
}

TransCoder::TransCoder(const Config_t& cfg)
    : config(cfg)
{
    init();
}

TransCoder::~TransCoder()
{
    if (capture) {
        delete capture;
        capture = nullptr;
    }
    if (rk_encoder) {
        delete rk_encoder;
        rk_encoder = nullptr;
    }
    for (auto* rkyolo : m_rkyolo_list) {
        delete rkyolo;
    }
    m_rkyolo_list.clear();
    if (m_pool) {
        delete m_pool;
        m_pool = nullptr;
    }
    free(encodeData);
    encodeData = nullptr;
}

std::vector<TransCoder::Config_t> TransCoder::LoadConfigs(const std::string& config_path)
{
    std::vector<Config_t> configs_out;
    INIReader configs(config_path);
    if (configs.ParseError() < 0) {
        LOG(ERROR, "read video config failed.");
        return configs_out;
    }

    const int default_width = configs.GetInteger("video", "width", 640);
    const int default_height = configs.GetInteger("video", "height", 480);
    const int default_fps = configs.GetInteger("video", "fps", 30);
    const int default_fix_qp = configs.GetInteger("video", "fix_qp", 23);
    const std::string default_device = configs.Get("video", "device", "/dev/video0");
    const int default_rknn_thread = configs.GetInteger("rknn", "rknn_thread", 3);
    const std::string default_stream_name = configs.Get("server", "stream_name", "unicast");

    bool has_multi_video = false;
    for (int i = 0; i < 8; ++i) {
        std::ostringstream sec;
        sec << "video" << i;
        if (configs.HasSection(sec.str())) {
            has_multi_video = true;
            break;
        }
    }

    if (has_multi_video) {
        for (int i = 0; i < 8; ++i) {
            std::ostringstream sec;
            sec << "video" << i;
            if (!configs.HasSection(sec.str())) continue;

            Config_t cfg{};
            cfg.section_name = sec.str();
            cfg.width = configs.GetInteger(sec.str(), "width", default_width);
            cfg.height = configs.GetInteger(sec.str(), "height", default_height);
            cfg.fps = configs.GetInteger(sec.str(), "fps", default_fps);
            cfg.fix_qp = configs.GetInteger(sec.str(), "fix_qp", default_fix_qp);
            cfg.rknn_thread = configs.GetInteger(sec.str(), "rknn_thread", default_rknn_thread);
            cfg.rknn_thread = (cfg.rknn_thread > 6) ? 6 : cfg.rknn_thread;
            cfg.device_name = configs.Get(sec.str(), "device", "");
            if (cfg.device_name.empty()) continue;
            cfg.stream_name = configs.Get(sec.str(), "stream_name", "cam" + std::to_string(i));
            configs_out.push_back(cfg);
        }
    }

    if (configs_out.empty()) {
        Config_t cfg{};
        cfg.section_name = "video";
        cfg.width = default_width;
        cfg.height = default_height;
        cfg.fps = default_fps;
        cfg.fix_qp = default_fix_qp;
        cfg.rknn_thread = (default_rknn_thread > 6) ? 6 : default_rknn_thread;
        cfg.device_name = default_device;
        cfg.stream_name = default_stream_name;
        configs_out.push_back(cfg);
    }

    return configs_out;
}

void TransCoder::init()
{
    std::list<uint32_t> formatList;
    v4l2IoType ioTypeIn = IOTYPE_MMAP;
    MppFrameFormat mpp_fmt;
    v4l2_buf_type buf_type;

    if (config.width <= 0) config.width = 640;
    if (config.height <= 0) config.height = 480;
    if (config.fps <= 0) config.fps = 30;
    if (config.fix_qp <= 0) config.fix_qp = 23;
    if (config.device_name.empty()) config.device_name = "/dev/video0";
    if (config.stream_name.empty()) config.stream_name = "unicast";
    config.rknn_thread = (config.rknn_thread > 6) ? 6 : std::max(1, config.rknn_thread);

    formatList.push_back(V4L2_PIX_FMT_YUYV);
    mpp_fmt = MPP_FMT_YUV420P;
    buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    V4L2DeviceParameters param(config.device_name.c_str(),
                               formatList,
                               config.width,
                               config.height,
                               config.fps,
                               ioTypeIn,
                               DEBUG);

    capture = V4l2Capture::create(param, buf_type);
    if (!capture) {
        LOG(ERROR, "[%s] create capture failed for device: %s",
            config.stream_name.c_str(), config.device_name.c_str());
        return;
    }
    encodeData = (uint8_t *)malloc(capture->getBufferSize());
    if (!encodeData) {
        LOG(ERROR, "[%s] alloc encode buffer failed", config.stream_name.c_str());
        return;
    }

    Encoder_Param_t encoder_param{
        mpp_fmt,
        config.width,
        config.height,
        0,
        0,
        0,
        config.fps,
        config.fix_qp};

    rk_encoder = new RkEncoder(encoder_param);
    if (!rk_encoder || rk_encoder->init() != 0) {
        LOG(ERROR, "[%s] init rk encoder failed", config.stream_name.c_str());
        return;
    }

    m_pool = new ThreadPool(config.rknn_thread);
    if (!m_pool) {
        LOG(ERROR, "[%s] create thread pool failed", config.stream_name.c_str());
        return;
    }

    for (int i = 0; i < config.rknn_thread; i++) {
        RkYolo *rkyolo = new RkYolo();
        if (!rkyolo || rkyolo->Init((rknn_core_mask)(1 << (i % 3))) != 0) {
            LOG(ERROR, "[%s] init yolo worker failed on slot %d", config.stream_name.c_str(), i);
            delete rkyolo;
            return;
        }
        m_rkyolo_list.push_back(rkyolo);
    }
    m_cur_yolo = 0;
    ready_ = true;
}

void TransCoder::run()
{
    if (!ready_ || !capture || !rk_encoder || !m_pool) {
        LOG(ERROR, "[%s] transcoder not ready, skip run", config.stream_name.c_str());
        return;
    }
    timeval tv;
    m_in_buffer_list.resize(config.rknn_thread, std::vector<uint8_t>(capture->getBufferSize()));
    m_out_buffer_list.resize(config.rknn_thread, std::vector<uint8_t>(capture->getBufferSize()));
    for (;;) {
        if (isStoped()) {
            break;
        }
        if (m_cur_yolo >= config.rknn_thread)
            m_cur_yolo = 0;

        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int startCode = 0;
        int ret = capture->isReadable(&tv);
        if (ret == 1) {
            const int slot = m_cur_yolo;
            int resize = capture->read((char *)m_in_buffer_list.at(slot).data(), m_in_buffer_list.at(slot).size());
            if (resize <= 0) {
                continue;
            }

            RkYolo *cur_yolo = m_rkyolo_list.at(slot);
            cur_yolo->SetBuffers(m_in_buffer_list.at(slot).data(), m_out_buffer_list.at(slot).data());

            m_cur_yolo++;
            frameSize = 0;

            PendingJob job;
            job.slot = slot;
            job.input_size = resize;
            job.future = m_pool->enqueue(&RkYolo::Inference, cur_yolo, config.width, config.height);
            pending_jobs_.push_back(std::move(job));

            if (pending_jobs_.size() >= (size_t)config.rknn_thread) {
                PendingJob finished = std::move(pending_jobs_.front());
                pending_jobs_.pop_front();
                int infer_ret = finished.future.get();
                if (infer_ret != 0) {
                    LOG(ERROR, "[%s] inference failed on slot %d ret=%d",
                        config.stream_name.c_str(), finished.slot, infer_ret);
                    continue;
                }
                frameSize = rk_encoder->encode(m_out_buffer_list.at(finished.slot).data(), finished.input_size, encodeData);
                LOG(INFO, "encodeData size %d", frameSize);
                if (rk_encoder->startCode3(encodeData))
                    startCode = 3;
                else
                    startCode = 4;

                if (onEncodedDataCallback && frameSize) {
                    onEncodedDataCallback(std::vector<uint8_t>(encodeData + startCode, encodeData + frameSize));
                }
            }
        } else if (ret == -1) {
            LOG(ERROR, "stop %s", strerror(errno));
        }
    }

    while (!pending_jobs_.empty()) {
        PendingJob finished = std::move(pending_jobs_.front());
        pending_jobs_.pop_front();
        int infer_ret = finished.future.get();
        if (infer_ret != 0) {
            continue;
        }
        frameSize = rk_encoder->encode(m_out_buffer_list.at(finished.slot).data(), finished.input_size, encodeData);
        if (frameSize <= 0) {
            continue;
        }
        int startCode = rk_encoder->startCode3(encodeData) ? 3 : 4;
        if (onEncodedDataCallback) {
            onEncodedDataCallback(std::vector<uint8_t>(encodeData + startCode, encodeData + frameSize));
        }
    }
}

TransCoder::Config_t const &TransCoder::getConfig() const
{
    return config;
}

bool TransCoder::isReady() const
{
    return ready_;
}

void TransCoder::setOnEncoderDataCallback(std::function<void(std::vector<uint8_t> &&)> callback)
{
    onEncodedDataCallback = std::move(callback);
}
