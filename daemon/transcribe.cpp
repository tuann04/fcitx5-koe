#include "transcribe.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include <nlohmann/json.hpp>

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

void writeLE16(std::string &out, uint16_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
}

void writeLE32(std::string &out, uint32_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 24) & 0xff));
}

size_t writeCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    std::string *out = static_cast<std::string *>(userdata);
    const size_t total = size * nmemb;
    out->append(ptr, total);
    return total;
}

int progressCallback(void *clientp, curl_off_t, curl_off_t, curl_off_t,
                     curl_off_t) {
    const auto *stop = static_cast<const std::atomic<bool> *>(clientp);
    return stop->load() ? 1 : 0;
}

std::string endpointUrl(const std::string &baseUrl, const std::string &path) {
    std::string base = baseUrl;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base + path;
}

} // namespace

std::string encodeWav(const std::vector<float> &pcm) {
    constexpr uint32_t sampleRate = 16000;

    std::string samples;
    samples.reserve(pcm.size() * 2);
    for (float value : pcm) {
        if (value > 1.0f) {
            value = 1.0f;
        } else if (value < -1.0f) {
            value = -1.0f;
        }
        const int16_t sample = static_cast<int16_t>(std::lround(value * 32767.0f));
        samples.push_back(static_cast<char>(sample & 0xff));
        samples.push_back(static_cast<char>((sample >> 8) & 0xff));
    }

    const uint32_t dataSize = static_cast<uint32_t>(samples.size());
    const uint32_t byteRate = sampleRate * 2;

    std::string wav;
    wav.reserve(44 + samples.size());
    wav += "RIFF";
    writeLE32(wav, 36 + dataSize);
    wav += "WAVE";
    wav += "fmt ";
    writeLE32(wav, 16);
    writeLE16(wav, 1);
    writeLE16(wav, 1);
    writeLE32(wav, sampleRate);
    writeLE32(wav, byteRate);
    writeLE16(wav, 2);
    writeLE16(wav, 16);
    wav += "data";
    writeLE32(wav, dataSize);
    wav += samples;
    return wav;
}

std::string stripAsrText(std::string text) {
    const std::string marker = "<asr_text>";
    const size_t pos = text.find(marker);
    if (pos != std::string::npos) {
        text.erase(0, pos + marker.size());
    }
    return text;
}

Transcriber::Transcriber(const Profile &profile) : profile_(profile) {
    curl_ = curl_easy_init();
}

Transcriber::~Transcriber() {
    if (curl_) {
        curl_easy_cleanup(curl_);
    }
}

TranscribeResult Transcriber::run(const std::vector<float> &pcm,
                                  const std::string &lang,
                                  const std::atomic<bool> &stop) {
    TranscribeResult result;
    if (!curl_) {
        result.error = "curl init failed";
        return result;
    }

    const std::string wav = encodeWav(pcm);
    const std::string url = endpointUrl(profile_.baseUrl, "/v1/audio/transcriptions");

    std::string response;
    curl_easy_reset(curl_);
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, static_cast<long>(profile_.timeoutSec));
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl_, CURLOPT_XFERINFOFUNCTION, progressCallback);
    curl_easy_setopt(curl_, CURLOPT_XFERINFODATA,
                     static_cast<const void *>(&stop));

    curl_slist *headers = nullptr;
    if (!profile_.apiKey.empty()) {
        const std::string auth = "Authorization: Bearer " + profile_.apiKey;
        headers = curl_slist_append(headers, auth.c_str());
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
    }

    curl_mime *mime = curl_mime_init(curl_);
    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_data(part, wav.data(), wav.size());
    curl_mime_filename(part, "audio.wav");
    curl_mime_type(part, "audio/wav");

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "model");
    curl_mime_data(part, profile_.model.c_str(), CURL_ZERO_TERMINATED);

    if (!lang.empty() && lang != "auto") {
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "language");
        curl_mime_data(part, lang.c_str(), CURL_ZERO_TERMINATED);
    }
    if (!profile_.prompt.empty()) {
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "prompt");
        curl_mime_data(part, profile_.prompt.c_str(), CURL_ZERO_TERMINATED);
    }

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "response_format");
    curl_mime_data(part, "json", CURL_ZERO_TERMINATED);

    curl_easy_setopt(curl_, CURLOPT_MIMEPOST, mime);

    const CURLcode code = curl_easy_perform(curl_);
    long httpCode = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_mime_free(mime);
    if (headers) {
        curl_slist_free_all(headers);
    }

    if (code != CURLE_OK) {
        result.error = curl_easy_strerror(code);
        return result;
    }
    if (httpCode < 200 || httpCode >= 300) {
        std::string body = response;
        for (char &c : body) {
            if (c == '\n' || c == '\r') {
                c = ' ';
            }
        }
        if (body.size() > 200) {
            size_t cut = 200;
            while (cut > 0 &&
                   (static_cast<unsigned char>(body[cut]) & 0xC0) == 0x80) {
                --cut;
            }
            body.resize(cut);
        }
        result.error = "HTTP " + std::to_string(httpCode) + ": " + body;
        return result;
    }

    std::string text;
    try {
        const nlohmann::json json = nlohmann::json::parse(response);
        if (!json.contains("text") || !json["text"].is_string()) {
            result.error = "bad response";
            return result;
        }
        text = json["text"].get<std::string>();
    } catch (const std::exception &) {
        result.error = "bad response";
        return result;
    }

    result.ok = true;
    result.text = trimAscii(stripAsrText(std::move(text)));
    return result;
}

} // namespace koe
