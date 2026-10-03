#ifndef KOE_DAEMON_TRANSCRIBE_H
#define KOE_DAEMON_TRANSCRIBE_H

#include "config.h"

#include <curl/curl.h>

#include <atomic>
#include <string>
#include <vector>

namespace koe {

struct TranscribeResult {
    bool ok = false;
    std::string text;
    std::string error;
};

std::string encodeWav(const std::vector<float> &pcm);

std::string stripAsrText(std::string text);

class Transcriber {
public:
    explicit Transcriber(const Profile &profile);
    ~Transcriber();

    TranscribeResult run(const std::vector<float> &pcm, const std::string &lang,
                         const std::atomic<bool> &stop);

private:
    Profile profile_;
    CURL *curl_ = nullptr;
};

} // namespace koe

#endif
