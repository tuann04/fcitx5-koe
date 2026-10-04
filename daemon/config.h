#ifndef KOE_DAEMON_CONFIG_H
#define KOE_DAEMON_CONFIG_H

#include <string>

namespace koe {

struct Profile {
    std::string baseUrl;
    std::string model;
    int timeoutSec = 15;
    int partialIntervalMs = 0;
    std::string prompt;
    std::string apiKey;
};

struct Config {
    std::string active = "local";
    int maxRecordSec = 120;
    float minRms = 0.005f;
    std::string saveDir;
    double saveMinSec = 0.0;
    double saveKeepDays = 0.0;
    Profile profile;
};

bool loadConfig(const std::string &overridePath, Config &out, bool &missing,
                std::string &err);

} // namespace koe

#endif
