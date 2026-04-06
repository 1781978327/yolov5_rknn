#include "CamFramedSource.h"
#include "log.h"

namespace {

bool hasNaluType(const std::vector<uint8_t>& data, uint8_t targetType)
{
    if (data.empty()) return false;
    if ((data[0] & 0x1f) == targetType) return true;

    for (size_t i = 0; i + 3 < data.size(); ++i) {
        size_t nalPos = data.size();
        if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
            nalPos = i + 3;
        } else if (i + 4 < data.size() &&
                   data[i] == 0x00 && data[i + 1] == 0x00 &&
                   data[i + 2] == 0x00 && data[i + 3] == 0x01) {
            nalPos = i + 4;
        }
        if (nalPos < data.size() &&
            (data[nalPos] & 0x1f) == targetType) {
            return true;
        }
    }
    return false;
}

} // namespace

CamFramedSource *CamFramedSource::createNew(UsageEnvironment &env, TransCoder &transcoder)
{
    return new CamFramedSource(env, transcoder);
}

CamFramedSource::CamFramedSource(UsageEnvironment &env, TransCoder &transcoder) :
    FramedSource(env),
    transcoder_(transcoder),
    eventTriggerId(0),
    waitForIdr_(true),
    max_nalu_size_bytes(0)
{
    // create trigger invoking method which will deliver frame
    eventTriggerId = envir().taskScheduler().createEventTrigger(CamFramedSource::deliverFrame0);
    encodedDataBuffer.reserve(5); // reserve enough space for handling incoming encoded data

    // set transcoder's callback indicating new encoded data availabity
    transcoder_.setOnEncoderDataCallback(std::bind(&CamFramedSource::onEncodedData, this, std::placeholders::_1));

    LOG(INFO, "Starting to capture and encoder video from video %s",
        transcoder_.getConfig().device_name.c_str());

    transcoder_.start();
    transcoder_.detach();
}

CamFramedSource::~CamFramedSource()
{
    transcoder_.stop();

    envir().taskScheduler().deleteEventTrigger(eventTriggerId);
    encodedDataBuffer.clear();
    encodedDataBuffer.shrink_to_fit();
}

void CamFramedSource::onEncodedData(std::vector<uint8_t> &&data)
{
    if (!isCurrentlyAwaitingData()) {
        waitForIdr_ = true;
        return;
    }

    if (waitForIdr_) {
        if (!hasNaluType(data, 5)) {
            return;
        }
        waitForIdr_ = false;
    }

    encodedDataMutex.lock();
    encodedDataBuffer.emplace_back(std::move(data));
    encodedDataMutex.unlock();

    envir().taskScheduler().triggerEvent(eventTriggerId, this);
}

void CamFramedSource::deliverFrame0(void *clientData)
{
    ((CamFramedSource *)clientData)->deliverData();
}

void CamFramedSource::deliverData()
{
    if (!isCurrentlyAwaitingData()) {
        return;
    }

    encodedDataMutex.lock();
    encodedData = std::move(encodedDataBuffer.front());
    encodedDataBuffer.erase(encodedDataBuffer.begin());
    encodedDataMutex.unlock();

    if (encodedData.size() > max_nalu_size_bytes) {
        max_nalu_size_bytes = encodedData.size();
    }

    if (encodedData.size() > fMaxSize) {
        fFrameSize = fMaxSize;
        fNumTruncatedBytes = static_cast<unsigned int>(encodedData.size() - fMaxSize);
        LOG(WARN, "Exceeded max size, truncated: %d, size: %d", fNumTruncatedBytes, encodedData.size());
    } else {
        fFrameSize = static_cast<unsigned int>(encodedData.size());
    }

    // can be changed to the actual frame's captured time
    gettimeofday(&fPresentationTime, nullptr);

    // DO NOT CHANGE ADDRESS, ONLY COPY (see Live555 docs)
    memcpy(fTo, encodedData.data(), fFrameSize);

    // should be invoked after successfully getting data
    FramedSource::afterGetting(this);
}

void CamFramedSource::doGetNextFrame()
{
    if (!encodedDataBuffer.empty()) {
        deliverData();
    } else {
        fFrameSize = 0;
        return;
    }
}

void CamFramedSource::doStopGettingFrames()
{
    waitForIdr_ = true;
    LOG(NOTICE, "Stop getting frames from the camera: %s", transcoder_.getConfig().device_name.c_str());
    FramedSource::doStopGettingFrames();
}
