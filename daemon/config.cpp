#include "config.h"

#include <sys/stat.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include <toml++/toml.h>

namespace koe {
namespace {

const char *kWhitespace = " \t\r\n\v\f";

std::string trimAscii(std::string text) {
    const size_t begin = text.find_first_not_of(kWhitespace);
    if (begin == std::string::npos) {
        return {};
    }
    const size_t end = text.find_last_not_of(kWhitespace);
    return text.substr(begin, end - begin + 1);
}

std::string defaultConfigPath() {
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        return std::string(xdg) + "/koe/config.toml";
    }
    const char *home = std::getenv("HOME");
    if (home && *home) {
        return std::string(home) + "/.config/koe/config.toml";
    }
    return {};
}

bool fileExists(const std::string &path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

std::string expandHome(const std::string &path) {
    if (!path.empty() && path[0] == '~') {
        const char *home = std::getenv("HOME");
        if (home && *home) {
            return std::string(home) + path.substr(1);
        }
    }
    return path;
}

Config builtinDefault() {
    Config config;
    config.active = "local";
    config.maxRecordSec = 120;
    config.profile.baseUrl = "http://127.0.0.1:8178";
    config.profile.model = "qwen3-asr-1.7b";
    config.profile.timeoutSec = 15;
    config.profile.partialIntervalMs = 700;
    return config;
}

std::string readApiKey(const std::string &path, std::string &err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "cannot read api_key_file '" + path + "'";
        return {};
    }
    std::string key((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    key = trimAscii(std::move(key));
    if (key.empty()) {
        err = "api_key_file '" + path + "' is empty";
    }
    return key;
}

} // namespace

bool loadConfig(const std::string &overridePath, Config &out, bool &missing,
                std::string &err) {
    missing = false;
    const std::string path =
        overridePath.empty() ? defaultConfigPath() : overridePath;
    if (path.empty()) {
        err = "cannot determine config path (XDG_CONFIG_HOME and HOME unset)";
        return false;
    }

    if (!fileExists(path)) {
        out = builtinDefault();
        missing = true;
        return true;
    }

    toml::table table;
    try {
        table = toml::parse_file(path);
    } catch (const toml::parse_error &e) {
        std::ostringstream os;
        os << "failed to parse " << path << ": " << e.description();
        err = os.str();
        return false;
    } catch (const std::exception &e) {
        err = "failed to parse " + path + ": " + e.what();
        return false;
    }

    const std::string active = table["active"].value_or<std::string>("");
    if (active.empty()) {
        err = path + ": 'active' is required";
        return false;
    }

    const toml::table *profiles = table["profile"].as_table();
    if (!profiles) {
        err = path + ": missing [profile] section";
        return false;
    }

    const toml::table *profile = (*profiles)[active].as_table();
    if (!profile) {
        err = path + ": active profile '" + active + "' not found";
        return false;
    }

    Config config;
    config.active = active;
    config.maxRecordSec =
        static_cast<int>(table["max_record_sec"].value_or<int64_t>(120));
    config.minRms = static_cast<float>(table["min_rms"].value_or<double>(0.005));

    config.profile.baseUrl = (*profile)["base_url"].value_or<std::string>("");
    config.profile.model = (*profile)["model"].value_or<std::string>("");
    config.profile.timeoutSec =
        static_cast<int>((*profile)["timeout_sec"].value_or<int64_t>(15));
    config.profile.partialIntervalMs = static_cast<int>(
        (*profile)["partial_interval_ms"].value_or<int64_t>(0));
    config.profile.prompt = (*profile)["prompt"].value_or<std::string>("");

    if (config.profile.baseUrl.empty()) {
        err = path + ": profile '" + active + "' is missing base_url";
        return false;
    }
    if (config.profile.model.empty()) {
        err = path + ": profile '" + active + "' is missing model";
        return false;
    }
    if (config.profile.timeoutSec <= 0) {
        err = path + ": timeout_sec must be positive";
        return false;
    }
    if (config.profile.partialIntervalMs < 0) {
        err = path + ": partial_interval_ms must be >= 0";
        return false;
    }
    if (config.profile.partialIntervalMs > 0 &&
        config.profile.partialIntervalMs < 300) {
        err = path +
              ": partial_interval_ms must be 0 (disabled) or >= 300 "
              "(values below 300 are too aggressive)";
        return false;
    }
    if (config.maxRecordSec <= 0) {
        err = path + ": max_record_sec must be positive";
        return false;
    }
    if (config.minRms < 0.0f) {
        err = path + ": min_rms must be >= 0";
        return false;
    }

    const std::string keyFile =
        expandHome((*profile)["api_key_file"].value_or<std::string>(""));
    if (!keyFile.empty()) {
        std::string keyErr;
        config.profile.apiKey = readApiKey(keyFile, keyErr);
        if (!keyErr.empty()) {
            err = path + ": " + keyErr;
            return false;
        }
    }

    out = std::move(config);
    return true;
}

} // namespace koe
