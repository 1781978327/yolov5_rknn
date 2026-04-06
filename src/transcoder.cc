#include <algorithm>
#include <list>
#include <string.h>
#include <vector>
#include <functional>
#include <sstream>
#include <cerrno>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "transcoder.h"
#include "INIReader.h"
#include "log.h"

#if __has_include(<linux/dma-heap.h>)
#include <linux/dma-heap.h>
#else
#ifndef DMA_HEAP_IOC_MAGIC
#define DMA_HEAP_IOC_MAGIC 'H'
struct dma_heap_allocation_data {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)
#endif
#endif

namespace {

static TransCoder::TrackerType parse_tracker_type(const std::string& tracker_type, bool tracker_enable)
{
    if (!tracker_enable) {
        return TransCoder::TrackerType::NONE;
    }
    if (tracker_type == "deepsort") {
        return TransCoder::TrackerType::DEEPSORT;
    }
    if (tracker_type == "none" || tracker_type == "off" || tracker_type == "disable") {
        return TransCoder::TrackerType::NONE;
    }
    return TransCoder::TrackerType::BYTETRACK;
}

static rknn_core_mask parse_rknn_core_mask(int core_id)
{
    switch (core_id) {
    case 0:
        return RKNN_NPU_CORE_0;
    case 1:
        return RKNN_NPU_CORE_1;
    case 2:
        return RKNN_NPU_CORE_2;
    default:
        return RKNN_NPU_CORE_2;
    }
}

static int dma_buf_alloc(const std::string& heap_path, size_t size, int* fd_out, void** va_out)
{
    int heap_fd = open(heap_path.c_str(), O_RDWR | O_CLOEXEC);
    if (heap_fd < 0) {
        return -1;
    }

    struct dma_heap_allocation_data alloc_data;
    memset(&alloc_data, 0, sizeof(alloc_data));
    alloc_data.len = size;
    alloc_data.fd_flags = O_RDWR | O_CLOEXEC;
    alloc_data.heap_flags = 0;

    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc_data) < 0) {
        close(heap_fd);
        return -1;
    }
    close(heap_fd);

    void* va = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, alloc_data.fd, 0);
    if (va == MAP_FAILED) {
        close(alloc_data.fd);
        return -1;
    }

    *fd_out = alloc_data.fd;
    *va_out = va;
    return 0;
}

static void dma_buf_free(int fd, void* va, size_t size)
{
    if (va && va != MAP_FAILED) {
        munmap(va, size);
    }
    if (fd >= 0) {
        close(fd);
    }
}

static std::string fourcc_to_string(uint32_t v)
{
    std::string s(4, ' ');
    s[0] = static_cast<char>(v & 0xFF);
    s[1] = static_cast<char>((v >> 8) & 0xFF);
    s[2] = static_cast<char>((v >> 16) & 0xFF);
    s[3] = static_cast<char>((v >> 24) & 0xFF);
    return s;
}

static void v4l2_dmabuf_close(TransCoder::V4l2DmabufCapture_t* cap)
{
    if (!cap) return;
    if (cap->fd >= 0 && cap->streaming) {
        int type = cap->buf_type;
        ioctl(cap->fd, VIDIOC_STREAMOFF, &type);
    }
    cap->streaming = false;
    if (cap->fd >= 0) {
        close(cap->fd);
        cap->fd = -1;
    }
    for (auto& b : cap->buffers) {
        dma_buf_free(b.fd, b.va, b.size);
        b.fd = -1;
        b.va = nullptr;
        b.size = 0;
    }
    cap->buffers.clear();
}

static int v4l2_dmabuf_qbuf(TransCoder::V4l2DmabufCapture_t* cap, int index)
{
    if (!cap || cap->fd < 0 || index < 0 || index >= (int)cap->buffers.size()) return -1;

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = cap->buf_type;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.index = static_cast<uint32_t>(index);

    if (cap->mplane) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        memset(planes, 0, sizeof(planes));
        buf.length = static_cast<uint32_t>(cap->num_planes);
        buf.m.planes = planes;
        planes[0].m.fd = cap->buffers[index].fd;
        planes[0].length = static_cast<uint32_t>(cap->plane0_size);
        planes[0].bytesused = static_cast<uint32_t>(cap->plane0_size);
    } else {
        buf.length = static_cast<uint32_t>(cap->plane0_size);
        buf.m.fd = cap->buffers[index].fd;
    }

    return ioctl(cap->fd, VIDIOC_QBUF, &buf);
}

static int v4l2_dmabuf_open(const TransCoder::Config_t& config,
                            TransCoder::V4l2DmabufCapture_t* out,
                            std::string* err)
{
    if (!out) return -1;
    *out = TransCoder::V4l2DmabufCapture_t{};

    int fd = open(config.device_name.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        if (err) *err = "open failed";
        return -1;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
        if (err) *err = "VIDIOC_QUERYCAP failed";
        close(fd);
        return -1;
    }

    uint32_t caps = cap.capabilities;
    if (caps & V4L2_CAP_DEVICE_CAPS) caps = cap.device_caps;
    if (!(caps & V4L2_CAP_STREAMING)) {
        if (err) *err = "device does not support streaming";
        close(fd);
        return -1;
    }

    int buf_type = -1;
    bool mplane = false;
    if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
        buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        mplane = true;
    } else if (caps & V4L2_CAP_VIDEO_CAPTURE) {
        buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        mplane = false;
    } else {
        if (err) *err = "device has no VIDEO_CAPTURE capability";
        close(fd);
        return -1;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = buf_type;
    if (mplane) {
        fmt.fmt.pix_mp.width = static_cast<uint32_t>(config.width);
        fmt.fmt.pix_mp.height = static_cast<uint32_t>(config.height);
        fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
    } else {
        fmt.fmt.pix.width = static_cast<uint32_t>(config.width);
        fmt.fmt.pix.height = static_cast<uint32_t>(config.height);
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field = V4L2_FIELD_ANY;
    }
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
        if (err) *err = "VIDIOC_S_FMT failed";
        close(fd);
        return -1;
    }

    const uint32_t actual_pixfmt = mplane ? fmt.fmt.pix_mp.pixelformat : fmt.fmt.pix.pixelformat;
    if (actual_pixfmt != V4L2_PIX_FMT_YUYV) {
        if (err) *err = "camera returned unsupported pixfmt=" + fourcc_to_string(actual_pixfmt);
        close(fd);
        return -1;
    }

    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = buf_type;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = static_cast<uint32_t>(std::max(1, config.fps));
    (void)ioctl(fd, VIDIOC_S_PARM, &parm);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = static_cast<uint32_t>(std::max(2, config.dma_buffers));
    req.type = buf_type;
    req.memory = V4L2_MEMORY_DMABUF;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 2) {
        if (err) *err = "VIDIOC_REQBUFS failed";
        close(fd);
        return -1;
    }

    const uint32_t fmt_width = mplane ? fmt.fmt.pix_mp.width : fmt.fmt.pix.width;
    const uint32_t fmt_height = mplane ? fmt.fmt.pix_mp.height : fmt.fmt.pix.height;
    const uint32_t fmt_sizeimage = mplane ? fmt.fmt.pix_mp.plane_fmt[0].sizeimage : fmt.fmt.pix.sizeimage;
    const size_t plane0_size = static_cast<size_t>(
        std::max<uint32_t>(fmt_sizeimage, static_cast<uint32_t>(fmt_width * fmt_height * 2)));

    out->fd = fd;
    out->buf_type = buf_type;
    out->mplane = mplane;
    out->width = static_cast<int>(fmt_width);
    out->height = static_cast<int>(fmt_height);
    out->pixfmt = actual_pixfmt;
    out->num_planes = mplane ? static_cast<int>(fmt.fmt.pix_mp.num_planes) : 1;
    out->plane0_size = plane0_size;
    out->buffers.resize(req.count);

    for (uint32_t i = 0; i < req.count; ++i) {
        auto& b = out->buffers[i];
        b.size = plane0_size;
        if (dma_buf_alloc(config.dma_heap, plane0_size, &b.fd, &b.va) != 0) {
            if (err) *err = "dma_buf_alloc failed";
            v4l2_dmabuf_close(out);
            return -1;
        }
        if (v4l2_dmabuf_qbuf(out, static_cast<int>(i)) != 0) {
            if (err) *err = "VIDIOC_QBUF failed";
            v4l2_dmabuf_close(out);
            return -1;
        }
    }

    int type = buf_type;
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        if (err) *err = "VIDIOC_STREAMON failed";
        v4l2_dmabuf_close(out);
        return -1;
    }

    out->streaming = true;
    return 0;
}

static int v4l2_dmabuf_dqbuf(TransCoder::V4l2DmabufCapture_t* cap, int timeout_ms, int* index_out)
{
    if (!cap || cap->fd < 0 || !index_out) return -1;
    *index_out = -1;

    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = cap->fd;
    pfd.events = POLLIN | POLLERR;
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr == 0) return 0;
    if (pr < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }
    if (pfd.revents & POLLERR) return -1;

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = cap->buf_type;
    buf.memory = V4L2_MEMORY_DMABUF;

    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    if (cap->mplane) {
        memset(planes, 0, sizeof(planes));
        buf.length = static_cast<uint32_t>(cap->num_planes);
        buf.m.planes = planes;
    }

    if (ioctl(cap->fd, VIDIOC_DQBUF, &buf) != 0) {
        if (errno == EAGAIN || errno == EINTR) return 0;
        return -1;
    }
    if (buf.index >= cap->buffers.size()) return -1;
    *index_out = static_cast<int>(buf.index);
    return 1;
}

} // namespace

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
    v4l2_dmabuf_close(&dmabuf_capture_);
    for (auto& b : output_dma_buffers_) {
        dma_buf_free(b.fd, b.va, b.size);
        b.fd = -1;
        b.va = nullptr;
        b.size = 0;
    }
    output_dma_buffers_.clear();
    if (rk_encoder) {
        delete rk_encoder;
        rk_encoder = nullptr;
    }
    if (byte_tracker_) {
        delete byte_tracker_;
        byte_tracker_ = nullptr;
    }
    if (deep_sort_tracker_) {
        delete deep_sort_tracker_;
        deep_sort_tracker_ = nullptr;
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
    const int default_dma_buffers = configs.GetInteger("video", "dma_buffers", 4);
    const std::string default_device = configs.Get("video", "device", "/dev/video0");
    const std::string default_dma_heap = configs.Get("video", "dma_heap", "/dev/dma_heap/system");
    const int default_rknn_thread = configs.GetInteger("rknn", "rknn_thread", 3);
    const std::string default_stream_name = configs.Get("server", "stream_name", "unicast");
    const bool default_tracker_enable = configs.GetBoolean("tracker", "enable", true);
    const std::string default_tracker_type = configs.Get("tracker", "type", "bytetrack");
    const int default_track_buffer = configs.GetInteger("tracker", "track_buffer", 30);
    const float default_track_thresh = static_cast<float>(configs.GetReal("tracker", "track_thresh", 0.5));
    const float default_high_thresh = static_cast<float>(configs.GetReal("tracker", "high_thresh", 0.6));
    const float default_match_thresh = static_cast<float>(configs.GetReal("tracker", "match_thresh", 0.8));
    const std::string default_reid_model_path = configs.Get("tracker", "reid_model_path", "model/osnet_x0_25_market.rknn");
    const int default_reid_feature_dim = configs.GetInteger("tracker", "reid_feature_dim", 512);
    const int default_reid_interval = configs.GetInteger("tracker", "reid_interval", 1);
    const int default_reid_cpu_id = configs.GetInteger("tracker", "reid_cpu_id", 6);
    const int default_reid_npu_core = configs.GetInteger("tracker", "reid_npu_core", 2);

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
            cfg.dma_buffers = std::max(2, (int)configs.GetInteger(sec.str(), "dma_buffers", default_dma_buffers));
            cfg.rknn_thread = configs.GetInteger(sec.str(), "rknn_thread", default_rknn_thread);
            cfg.rknn_thread = (cfg.rknn_thread > 6) ? 6 : cfg.rknn_thread;
            cfg.tracker_enable = configs.GetBoolean(sec.str(), "tracker_enable", default_tracker_enable);
            cfg.tracker_type = configs.Get(sec.str(), "tracker_type", default_tracker_type);
            cfg.track_buffer = std::max(1, (int)configs.GetInteger(sec.str(), "track_buffer", default_track_buffer));
            cfg.track_thresh = static_cast<float>(configs.GetReal(sec.str(), "track_thresh", default_track_thresh));
            cfg.high_thresh = static_cast<float>(configs.GetReal(sec.str(), "high_thresh", default_high_thresh));
            cfg.match_thresh = static_cast<float>(configs.GetReal(sec.str(), "match_thresh", default_match_thresh));
            cfg.reid_model_path = configs.Get(sec.str(), "reid_model_path", default_reid_model_path);
            cfg.reid_feature_dim = std::max(1, (int)configs.GetInteger(sec.str(), "reid_feature_dim", default_reid_feature_dim));
            cfg.reid_interval = std::max(1, (int)configs.GetInteger(sec.str(), "reid_interval", default_reid_interval));
            cfg.reid_cpu_id = (int)configs.GetInteger(sec.str(), "reid_cpu_id", default_reid_cpu_id);
            cfg.reid_npu_core = (int)configs.GetInteger(sec.str(), "reid_npu_core", default_reid_npu_core);
            cfg.device_name = configs.Get(sec.str(), "device", "");
            if (cfg.device_name.empty()) continue;
            cfg.stream_name = configs.Get(sec.str(), "stream_name", "cam" + std::to_string(i));
            cfg.dma_heap = configs.Get(sec.str(), "dma_heap", default_dma_heap);
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
        cfg.dma_buffers = std::max(2, default_dma_buffers);
        cfg.rknn_thread = (default_rknn_thread > 6) ? 6 : default_rknn_thread;
        cfg.tracker_enable = configs.GetBoolean("video", "tracker_enable", default_tracker_enable);
        cfg.tracker_type = configs.Get("video", "tracker_type", default_tracker_type);
        cfg.track_buffer = std::max(1, (int)configs.GetInteger("video", "track_buffer", default_track_buffer));
        cfg.track_thresh = static_cast<float>(configs.GetReal("video", "track_thresh", default_track_thresh));
        cfg.high_thresh = static_cast<float>(configs.GetReal("video", "high_thresh", default_high_thresh));
        cfg.match_thresh = static_cast<float>(configs.GetReal("video", "match_thresh", default_match_thresh));
        cfg.reid_model_path = configs.Get("video", "reid_model_path", default_reid_model_path);
        cfg.reid_feature_dim = std::max(1, (int)configs.GetInteger("video", "reid_feature_dim", default_reid_feature_dim));
        cfg.reid_interval = std::max(1, (int)configs.GetInteger("video", "reid_interval", default_reid_interval));
        cfg.reid_cpu_id = (int)configs.GetInteger("video", "reid_cpu_id", default_reid_cpu_id);
        cfg.reid_npu_core = (int)configs.GetInteger("video", "reid_npu_core", default_reid_npu_core);
        cfg.device_name = default_device;
        cfg.stream_name = default_stream_name;
        cfg.dma_heap = default_dma_heap;
        configs_out.push_back(cfg);
    }

    return configs_out;
}

void TransCoder::init()
{
    MppFrameFormat mpp_fmt;

    if (config.width <= 0) config.width = 640;
    if (config.height <= 0) config.height = 480;
    if (config.fps <= 0) config.fps = 30;
    if (config.fix_qp <= 0) config.fix_qp = 23;
    if (config.dma_buffers < 2) config.dma_buffers = 4;
    if (config.device_name.empty()) config.device_name = "/dev/video0";
    if (config.stream_name.empty()) config.stream_name = "unicast";
    if (config.dma_heap.empty()) config.dma_heap = "/dev/dma_heap/system";
    config.rknn_thread = (config.rknn_thread > 6) ? 6 : std::max(1, config.rknn_thread);
    config.track_buffer = std::max(1, config.track_buffer);
    if (config.tracker_type.empty()) config.tracker_type = "bytetrack";
    if (config.reid_model_path.empty()) config.reid_model_path = "model/osnet_x0_25_market.rknn";
    config.reid_feature_dim = std::max(1, config.reid_feature_dim);
    config.reid_interval = std::max(1, config.reid_interval);
    tracker_type_ = parse_tracker_type(config.tracker_type, config.tracker_enable);

    mpp_fmt = MPP_FMT_YUV420P;

    std::string dmabuf_err;
    if (v4l2_dmabuf_open(config, &dmabuf_capture_, &dmabuf_err) != 0) {
        LOG(ERROR, "[%s] create dmabuf capture failed for device: %s (%s)",
            config.stream_name.c_str(), config.device_name.c_str(), dmabuf_err.c_str());
        v4l2_dmabuf_close(&dmabuf_capture_);
        return;
    }
    if (dmabuf_capture_.width != config.width || dmabuf_capture_.height != config.height) {
        LOG(ERROR, "[%s] camera returned unexpected size %dx%d (requested %dx%d)",
            config.stream_name.c_str(),
            dmabuf_capture_.width, dmabuf_capture_.height,
            config.width, config.height);
        v4l2_dmabuf_close(&dmabuf_capture_);
        return;
    }

    output_frame_size_ = config.width * config.height * 3 / 2;
    encodeData = (uint8_t *)malloc(std::max((int)dmabuf_capture_.plane0_size, output_frame_size_) + 4096);
    if (!encodeData) {
        LOG(ERROR, "[%s] alloc encode buffer failed", config.stream_name.c_str());
        v4l2_dmabuf_close(&dmabuf_capture_);
        return;
    }

    output_dma_buffers_.resize(config.rknn_thread);
    for (int i = 0; i < config.rknn_thread; ++i) {
        auto& b = output_dma_buffers_[i];
        b.size = output_frame_size_;
        if (dma_buf_alloc(config.dma_heap, b.size, &b.fd, &b.va) != 0) {
            LOG(ERROR, "[%s] alloc output dma buffer failed on slot %d", config.stream_name.c_str(), i);
            return;
        }
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
        v4l2_dmabuf_close(&dmabuf_capture_);
        return;
    }

    m_pool = new ThreadPool(config.rknn_thread);
    if (!m_pool) {
        LOG(ERROR, "[%s] create thread pool failed", config.stream_name.c_str());
        v4l2_dmabuf_close(&dmabuf_capture_);
        return;
    }

    for (int i = 0; i < config.rknn_thread; i++) {
        RkYolo *rkyolo = new RkYolo();
        if (!rkyolo || rkyolo->Init((rknn_core_mask)(1 << (i % 3))) != 0) {
            LOG(ERROR, "[%s] init yolo worker failed on slot %d", config.stream_name.c_str(), i);
            delete rkyolo;
            v4l2_dmabuf_close(&dmabuf_capture_);
            return;
        }
        m_rkyolo_list.push_back(rkyolo);
    }
    if (tracker_type_ == TrackerType::BYTETRACK) {
        byte_tracker_ = new ByteTrackerWrapper(config.fps, config.track_buffer,
                                               config.track_thresh, config.high_thresh, config.match_thresh);
        if (!byte_tracker_) {
            LOG(ERROR, "[%s] create bytetrack failed", config.stream_name.c_str());
            return;
        }
        LOG(NOTICE, "[%s] tracker=bytetrack buffer=%d track=%.2f high=%.2f match=%.2f",
            config.stream_name.c_str(), config.track_buffer,
            config.track_thresh, config.high_thresh, config.match_thresh);
    } else if (tracker_type_ == TrackerType::DEEPSORT) {
        FILE* fp = fopen(config.reid_model_path.c_str(), "rb");
        if (!fp) {
            LOG(ERROR, "[%s] deepsort model not found: %s",
                config.stream_name.c_str(), config.reid_model_path.c_str());
            return;
        }
        fclose(fp);

        deep_sort_tracker_ = new DeepSortWrapper(config.reid_model_path,
                                                 1,
                                                 config.reid_feature_dim,
                                                 config.reid_interval,
                                                 config.reid_cpu_id,
                                                 parse_rknn_core_mask(config.reid_npu_core));
        if (!deep_sort_tracker_) {
            LOG(ERROR, "[%s] create deepsort failed", config.stream_name.c_str());
            return;
        }
        LOG(NOTICE, "[%s] tracker=deepsort model=%s interval=%d cpu=%d npu_core=%d",
            config.stream_name.c_str(), config.reid_model_path.c_str(),
            config.reid_interval, config.reid_cpu_id, config.reid_npu_core);
    } else {
        LOG(NOTICE, "[%s] tracker=none", config.stream_name.c_str());
    }
    m_cur_yolo = 0;
    ready_ = true;
}

void TransCoder::run()
{
    if (!ready_ || !rk_encoder || !m_pool || dmabuf_capture_.fd < 0) {
        LOG(ERROR, "[%s] transcoder not ready, skip run", config.stream_name.c_str());
        return;
    }
    timeval tv;
    for (;;) {
        if (isStoped()) {
            break;
        }
        if (m_cur_yolo >= config.rknn_thread)
            m_cur_yolo = 0;

        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int startCode = 0;
        int capture_index = -1;
        int ret = v4l2_dmabuf_dqbuf(&dmabuf_capture_, 1000, &capture_index);
        if (ret == 1) {
            const int slot = m_cur_yolo;
            if (capture_index < 0 || capture_index >= (int)dmabuf_capture_.buffers.size()) {
                continue;
            }
            auto& capture_buf = dmabuf_capture_.buffers[capture_index];
            if (!capture_buf.va) {
                (void)v4l2_dmabuf_qbuf(&dmabuf_capture_, capture_index);
                continue;
            }

            RkYolo *cur_yolo = m_rkyolo_list.at(slot);
            cur_yolo->SetBuffers((uint8_t*)capture_buf.va,
                                 (uint8_t*)output_dma_buffers_.at(slot).va,
                                 output_dma_buffers_.at(slot).fd);

            m_cur_yolo++;
            frameSize = 0;

            PendingJob job;
            job.slot = slot;
            job.input_size = output_frame_size_;
            job.capture_index = capture_index;
            job.yolo = cur_yolo;
            job.future = m_pool->enqueue(&RkYolo::Inference, cur_yolo, config.width, config.height);
            pending_jobs_.push_back(std::move(job));

            if (pending_jobs_.size() >= (size_t)config.rknn_thread) {
                PendingJob finished = std::move(pending_jobs_.front());
                pending_jobs_.pop_front();
                int infer_ret = finished.future.get();
                if (infer_ret != 0) {
                    if (finished.capture_index >= 0) {
                        (void)v4l2_dmabuf_qbuf(&dmabuf_capture_, finished.capture_index);
                    }
                    LOG(ERROR, "[%s] inference failed on slot %d ret=%d",
                        config.stream_name.c_str(), finished.slot, infer_ret);
                    continue;
                }
                detect_result_group_t overlay_group{};
                if (finished.yolo) {
                    overlay_group = finished.yolo->GetLastDetectResult();
                }
                if (tracker_type_ == TrackerType::BYTETRACK && byte_tracker_) {
                    overlay_group = byte_tracker_->Update(overlay_group);
                } else if (tracker_type_ == TrackerType::DEEPSORT && deep_sort_tracker_) {
                    auto& capture_buf_done = dmabuf_capture_.buffers.at(finished.capture_index);
                    overlay_group = deep_sort_tracker_->Update(
                        static_cast<const uint8_t*>(capture_buf_done.va),
                        config.width, config.height, overlay_group);
                }
                if (finished.yolo) {
                    int render_ret = finished.yolo->RenderOverlay(overlay_group, config.width, config.height);
                    if (render_ret != 0) {
                        if (finished.capture_index >= 0) {
                            (void)v4l2_dmabuf_qbuf(&dmabuf_capture_, finished.capture_index);
                        }
                        LOG(ERROR, "[%s] render overlay failed on slot %d ret=%d",
                            config.stream_name.c_str(), finished.slot, render_ret);
                        continue;
                    }
                }
                if (finished.capture_index >= 0) {
                    (void)v4l2_dmabuf_qbuf(&dmabuf_capture_, finished.capture_index);
                }
                auto& out_dma = output_dma_buffers_.at(finished.slot);
                frameSize = rk_encoder->encode_dmabuf(out_dma.fd, out_dma.va,
                                                     config.width, config.height,
                                                     finished.input_size, encodeData);
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
            if (finished.capture_index >= 0) {
                (void)v4l2_dmabuf_qbuf(&dmabuf_capture_, finished.capture_index);
            }
            continue;
        }
        detect_result_group_t overlay_group{};
        if (finished.yolo) {
            overlay_group = finished.yolo->GetLastDetectResult();
        }
        if (tracker_type_ == TrackerType::BYTETRACK && byte_tracker_) {
            overlay_group = byte_tracker_->Update(overlay_group);
        } else if (tracker_type_ == TrackerType::DEEPSORT && deep_sort_tracker_) {
            auto& capture_buf_done = dmabuf_capture_.buffers.at(finished.capture_index);
            overlay_group = deep_sort_tracker_->Update(
                static_cast<const uint8_t*>(capture_buf_done.va),
                config.width, config.height, overlay_group);
        }
        if (finished.yolo) {
            int render_ret = finished.yolo->RenderOverlay(overlay_group, config.width, config.height);
            if (render_ret != 0) {
                if (finished.capture_index >= 0) {
                    (void)v4l2_dmabuf_qbuf(&dmabuf_capture_, finished.capture_index);
                }
                continue;
            }
        }
        if (finished.capture_index >= 0) {
            (void)v4l2_dmabuf_qbuf(&dmabuf_capture_, finished.capture_index);
        }
        auto& out_dma = output_dma_buffers_.at(finished.slot);
        frameSize = rk_encoder->encode_dmabuf(out_dma.fd, out_dma.va,
                                             config.width, config.height,
                                             finished.input_size, encodeData);
        if (frameSize <= 0) {
            continue;
        }
        int startCode = rk_encoder->startCode3(encodeData) ? 3 : 4;
        if (onEncodedDataCallback) {
            onEncodedDataCallback(std::vector<uint8_t>(encodeData + startCode, encodeData + frameSize));
        }
    }

    v4l2_dmabuf_close(&dmabuf_capture_);
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
