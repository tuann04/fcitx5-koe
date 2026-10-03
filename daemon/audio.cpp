#include "audio.h"

#include <algorithm>
#include <cstring>

#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

namespace koe {

Recorder::Recorder() = default;

Recorder::~Recorder() { shutdown(); }

bool Recorder::init(std::string &err) {
    pw_init(nullptr, nullptr);
    initialized_ = true;

    loop_ = pw_thread_loop_new("koe", nullptr);
    if (!loop_) {
        err = "failed to create PipeWire thread loop";
        return false;
    }

    pw_thread_loop_lock(loop_);
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (context_) {
        core_ = pw_context_connect(context_, nullptr, 0);
    }
    pw_thread_loop_unlock(loop_);

    if (!context_) {
        err = "failed to create PipeWire context";
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
        return false;
    }
    if (!core_) {
        err = "failed to connect to PipeWire";
        pw_thread_loop_lock(loop_);
        pw_context_destroy(context_);
        context_ = nullptr;
        pw_thread_loop_unlock(loop_);
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
        return false;
    }

    if (pw_thread_loop_start(loop_) < 0) {
        err = "failed to start PipeWire thread loop";
        pw_thread_loop_lock(loop_);
        pw_core_disconnect(core_);
        core_ = nullptr;
        pw_context_destroy(context_);
        context_ = nullptr;
        pw_thread_loop_unlock(loop_);
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
        return false;
    }

    return true;
}

void Recorder::setMaxSamples(size_t maxSamples) { maxSamples_ = maxSamples; }

bool Recorder::takeCapped() { return capped_.exchange(false); }

void Recorder::setErrorCallback(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(errorMutex_);
    errorCallback_ = std::move(callback);
}

bool Recorder::takeError(std::string &out) {
    if (!hasPendingError_.exchange(false)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(errorMutex_);
    out = pendingError_;
    pendingError_.clear();
    return true;
}

bool Recorder::start(std::string &err) {
    if (active_.load()) {
        err = "already recording";
        return false;
    }
    if (!loop_ || !core_) {
        err = "audio not initialized";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.clear();
    }
    capped_.store(false);

    pw_thread_loop_lock(loop_);

    pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Communication", PW_KEY_NODE_NAME, "koe",
        PW_KEY_APP_NAME, "koe", nullptr);
    pw_stream *stream = pw_stream_new(core_, "koe", props);
    if (!stream) {
        pw_thread_loop_unlock(loop_);
        err = "failed to create capture stream";
        return false;
    }

    static const pw_stream_events events = [] {
        pw_stream_events e{};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.process = &Recorder::onProcess;
        e.state_changed = &Recorder::onStateChanged;
        return e;
    }();

    spa_zero(streamListener_);
    stream_ = stream;
    pw_stream_add_listener(stream, &streamListener_, &events, this);

    uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(
        .format = SPA_AUDIO_FORMAT_F32, .rate = 16000, .channels = 1);
    info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    const spa_pod *params[1];
    params[0] =
        spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);

    const pw_stream_flags flags = static_cast<pw_stream_flags>(
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);
    const int rc =
        pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1);
    if (rc < 0) {
        stream_ = nullptr;
        pw_stream_destroy(stream);
        pw_thread_loop_unlock(loop_);
        err = std::string("failed to connect capture stream: ") +
              std::strerror(-rc);
        return false;
    }

    pw_thread_loop_unlock(loop_);
    active_.store(true);
    return true;
}

std::vector<float> Recorder::stop() {
    active_.store(false);
    destroyStreamLocked();

    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<float> out = std::move(samples_);
    samples_.clear();
    return out;
}

std::vector<float> Recorder::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_;
}

void Recorder::cancel() {
    active_.store(false);
    destroyStreamLocked();

    std::lock_guard<std::mutex> lock(mutex_);
    samples_.clear();
}

void Recorder::destroyStreamLocked() {
    if (!stream_) {
        return;
    }
    pw_thread_loop_lock(loop_);
    pw_stream_disconnect(stream_);
    pw_stream_destroy(stream_);
    stream_ = nullptr;
    pw_thread_loop_unlock(loop_);
}

void Recorder::shutdown() {
    active_.store(false);

    if (loop_) {
        pw_thread_loop_lock(loop_);
        if (stream_) {
            pw_stream_disconnect(stream_);
            pw_stream_destroy(stream_);
            stream_ = nullptr;
        }
        if (core_) {
            pw_core_disconnect(core_);
            core_ = nullptr;
        }
        if (context_) {
            pw_context_destroy(context_);
            context_ = nullptr;
        }
        pw_thread_loop_unlock(loop_);
        pw_thread_loop_stop(loop_);
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }

    if (initialized_) {
        pw_deinit();
        initialized_ = false;
    }
}

void Recorder::onProcess(void *data) {
    static_cast<Recorder *>(data)->process();
}

void Recorder::process() {
    pw_stream *stream = stream_;
    if (!stream) {
        return;
    }
    pw_buffer *buffer = pw_stream_dequeue_buffer(stream);
    if (!buffer) {
        return;
    }

    spa_buffer *buf = buffer->buffer;
    if (buf && buf->n_datas > 0 && buf->datas[0].data && buf->datas[0].chunk) {
        const uint32_t maxSize = buf->datas[0].maxsize;
        uint32_t offset = buf->datas[0].chunk->offset;
        if (offset > maxSize) {
            offset = maxSize;
        }
        uint32_t size = buf->datas[0].chunk->size;
        if (size > maxSize - offset) {
            size = maxSize - offset;
        }
        const uint8_t *base = static_cast<const uint8_t *>(buf->datas[0].data);
        const float *source =
            reinterpret_cast<const float *>(base + offset);
        const size_t count = size / sizeof(float);
        std::lock_guard<std::mutex> lock(mutex_);
        if (samples_.size() < maxSamples_) {
            const size_t room = maxSamples_ - samples_.size();
            const size_t take = std::min(room, count);
            samples_.insert(samples_.end(), source, source + take);
            if (take < count) {
                capped_.store(true);
            }
        } else {
            capped_.store(true);
        }
    }

    pw_stream_queue_buffer(stream, buffer);
}

void Recorder::onStateChanged(void *data, pw_stream_state old,
                              pw_stream_state state, const char *error) {
    (void)old;
    if (state != PW_STREAM_STATE_ERROR) {
        return;
    }

    Recorder *self = static_cast<Recorder *>(data);
    ErrorCallback callback;
    {
        std::lock_guard<std::mutex> lock(self->errorMutex_);
        self->pendingError_ = error ? error : "capture stream error";
        self->hasPendingError_.store(true);
        callback = self->errorCallback_;
    }
    if (callback) {
        callback();
    }
}

} // namespace koe
