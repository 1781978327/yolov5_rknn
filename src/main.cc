#include <iostream>
#include "CamRTSPServer.h"
#include "INIReader.h"
#include "log.h"

int LogLevel;
int main()
{
    INIReader configs("./configs/config.ini");
    if (configs.ParseError() < 0) {
        printf("read config failed.");
        exit(1);
    } else {
        std::string level = configs.Get("log", "level", "NOTICE");
        if (level == "NOTICE") {
            initLogger(NOTICE);
        } else if (level == "INFO") {
            initLogger(INFO);
        } else if (level == "ERROR") {
            initLogger(ERROR);
        }
    }

    try {
        CamRTSPServer server;
        std::vector<TransCoder::Config_t> transcoder_configs = TransCoder::LoadConfigs("./configs/config.ini");
        if (transcoder_configs.empty()) {
            LOG(ERROR, "No valid video config found.");
            return 1;
        }
        int ready_count = 0;
        for (const auto& cfg : transcoder_configs) {
            auto transcoder = std::make_shared<TransCoder>(cfg);
            if (!transcoder->isReady()) {
                LOG(ERROR, "Skip invalid transcoder config: device=%s stream=%s",
                    cfg.device_name.c_str(), cfg.stream_name.c_str());
                continue;
            }
            server.addTranscoder(transcoder);
            ready_count++;
        }
        if (ready_count == 0) {
            LOG(ERROR, "No ready transcoder available.");
            return 1;
        }
        server.run();
    } catch (const std::invalid_argument &err) {
        LOG(ERROR, err.what());
    }

    LOG(INFO, "test.");
    return 0;
}
