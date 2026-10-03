#ifndef KOE_DAEMON_AUDIO_H
#define KOE_DAEMON_AUDIO_H

#include <pipewire/pipewire.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace koe {

class Recorder {
public:
    Recorder();
    ~Recorder();

    Recorder(const Recorder &) = delete;
    Recorder &operator=(const Recorder &) = delete;

    bool init(std::string &err);
    void shutdown();

    void setMaxSamples(size_t maxSamples);

    bool start(std::string &err);
    std::vector<float> stop();
    std::vector<float> snapshot() const;
    void cancel();
    bool active() const { return active_.load(); }

    bool takeCapped();

    using ErrorCallback = std::function<void()>;
    void setErrorCallback(ErrorCallback callback);
    bool takeError(std::string &out);

private:
    static void onProcess(void *data);
    static void onStateChanged(void *data, pw_stream_state old,
                               pw_stream_state state, const char *error);
    void process();
    void destroyStreamLocked();

    pw_thread_loop *loop_ = nullptr;
    pw_context *context_ = nullptr;
    pw_core *core_ = nullptr;
    pw_stream *stream_ = nullptr;
    spa_hook streamListener_{};

    mutable std::mutex mutex_;
    std::vector<float> samples_;
    size_t maxSamples_ = 0;

    std::atomic<bool> active_{false};
    std::atomic<bool> capped_{false};
    bool initialized_ = false;

    std::mutex errorMutex_;
    std::string pendingError_;
    std::atomic<bool> hasPendingError_{false};
    ErrorCallback errorCallback_;
};

} // namespace koe

#endif
