#include "audio.h"
#include "config.h"
#include "transcribe.h"

#include "../common/protocol.h"

#include <curl/curl.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr size_t kMaxClientOut = 1 << 20;
constexpr size_t kMinClipSamples = 4800;
constexpr size_t kPartialMinSamples = 8000;

bool g_verbose = false;
std::mutex g_logMutex;

void logRaw(const char *level, const std::string &message) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::cerr << "koe-daemon: " << level << message << '\n';
}

void logInfo(const std::string &message) { logRaw("", message); }
void logError(const std::string &message) { logRaw("error: ", message); }

void logDebug(const std::string &message) {
    if (g_verbose) {
        logRaw("debug: ", message);
    }
}

std::string errnoString(int error) { return std::strerror(error); }

std::string formatSeconds(size_t samples) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", samples / 16000.0);
    return buffer;
}

double clipRms(const std::vector<float> &pcm) {
    double sum = 0.0;
    for (float value : pcm) {
        sum += static_cast<double>(value) * static_cast<double>(value);
    }
    return std::sqrt(sum / static_cast<double>(pcm.size()));
}

std::string formatRms(double rms) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6f", rms);
    return buffer;
}

struct Client {
    uint64_t cid = 0;
    int fd = -1;
    bool readClosed = false;
    koe::LineBuffer input;
    std::string out;
    size_t outSent = 0;
};

struct Job {
    uint64_t id = 0;
    uint64_t clientId = 0;
    std::string lang;
    std::vector<float> pcm;
    bool partial = false;
    std::atomic<bool> cancelled{false};
};

struct Result {
    std::shared_ptr<Job> job;
    bool cancelled = false;
    bool ok = false;
    std::string text;
    std::string error;
};

struct Recording {
    bool active = false;
    uint64_t id = 0;
    uint64_t clientId = 0;
    std::string lang;
};

class Daemon {
public:
    Daemon(koe::Config config, std::string socketPath)
        : config_(std::move(config)), socketPath_(std::move(socketPath)) {}

    ~Daemon() { cleanup(); }

    bool init(std::string &err) {
        if (pipe2(wake_, O_NONBLOCK | O_CLOEXEC) != 0) {
            err = std::string("pipe2: ") + errnoString(errno);
            return false;
        }

        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        sigFd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
        if (sigFd_ < 0) {
            err = std::string("signalfd: ") + errnoString(errno);
            return false;
        }

        if (!setupSocket(err)) {
            return false;
        }

        recorder_.setMaxSamples(static_cast<size_t>(config_.maxRecordSec) * 16000);
        std::string audioErr;
        if (!recorder_.init(audioErr)) {
            recorderUnavailable_ = true;
            recorderError_ = audioErr;
            logError("audio unavailable: " + audioErr);
        } else {
            recorder_.setErrorCallback([this]() { wake(); });
        }

        worker_ = std::thread([this]() { workerLoop(); });
        return true;
    }

    int run() {
        bool running = true;
        while (running) {
            std::vector<pollfd> fds;
            fds.push_back({listenFd_, POLLIN, 0});
            fds.push_back({wake_[0], POLLIN, 0});
            fds.push_back({sigFd_, POLLIN, 0});
            const size_t clientCount = clients_.size();
            for (const auto &client : clients_) {
                short events = client->readClosed ? 0 : POLLIN;
                if (!client->out.empty()) {
                    events |= POLLOUT;
                }
                fds.push_back({client->fd, events, 0});
            }

            const int rc = ::poll(fds.data(), fds.size(), pollTimeoutMs());
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                logError(std::string("poll: ") + errnoString(errno));
                break;
            }

            if (fds[2].revents & POLLIN) {
                signalfd_siginfo info;
                while (::read(sigFd_, &info, sizeof(info)) ==
                       static_cast<ssize_t>(sizeof(info))) {
                }
                logInfo("received signal, shutting down");
                running = false;
                break;
            }

            if (fds[1].revents & POLLIN) {
                drainWake();
                handleRecorderError();
                drainResults();
            }

            if (fds[0].revents & POLLIN) {
                acceptClients();
            }

            for (size_t i = 0; i < clientCount; ++i) {
                Client *client = clients_[i].get();
                const pollfd &fd = fds[3 + i];
                if (fd.revents == 0) {
                    continue;
                }
                if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    disconnectClient(client);
                    continue;
                }
                if (fd.revents & POLLOUT) {
                    flushClient(client);
                }
                if (client->fd >= 0 && (fd.revents & POLLIN)) {
                    readClient(client);
                }
            }

            clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                          [](const std::unique_ptr<Client> &c) {
                                              return c->fd < 0;
                                          }),
                           clients_.end());

            maybeSchedulePartial();
        }
        return 0;
    }

    void cleanup() {
        if (cleanedUp_) {
            return;
        }
        cleanedUp_ = true;

        stopFlag_.store(true);
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            stopWorker_ = true;
        }
        queueCv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }

        recorder_.shutdown();

        for (auto &client : clients_) {
            if (client->fd >= 0) {
                ::close(client->fd);
            }
        }
        clients_.clear();

        if (listenFd_ >= 0) {
            ::close(listenFd_);
            listenFd_ = -1;
        }
        if (sigFd_ >= 0) {
            ::close(sigFd_);
            sigFd_ = -1;
        }
        if (wake_[0] >= 0) {
            ::close(wake_[0]);
            wake_[0] = -1;
        }
        if (wake_[1] >= 0) {
            ::close(wake_[1]);
            wake_[1] = -1;
        }
        if (!socketPath_.empty() && socketCreated_) {
            ::unlink(socketPath_.c_str());
            socketCreated_ = false;
        }
    }

private:
    bool setupSocket(std::string &err) {
        sockaddr_un addr{};
        if (socketPath_.size() >= sizeof(addr.sun_path)) {
            err = "socket path too long: " + socketPath_;
            return false;
        }

        struct stat st;
        if (::stat(socketPath_.c_str(), &st) == 0) {
            const int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (probe >= 0) {
                addr.sun_family = AF_UNIX;
                std::strncpy(addr.sun_path, socketPath_.c_str(),
                             sizeof(addr.sun_path) - 1);
                const int connected =
                    ::connect(probe, reinterpret_cast<sockaddr *>(&addr),
                              sizeof(addr));
                ::close(probe);
                if (connected == 0) {
                    err = "another koe-daemon is already running on " +
                          socketPath_;
                    return false;
                }
            }
            if (::unlink(socketPath_.c_str()) != 0 && errno != ENOENT) {
                err = "unlink " + socketPath_ + ": " + errnoString(errno);
                return false;
            }
        }

        listenFd_ =
            ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listenFd_ < 0) {
            err = std::string("socket: ") + errnoString(errno);
            return false;
        }

        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socketPath_.c_str(), sizeof(addr.sun_path) - 1);
        if (::bind(listenFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) !=
            0) {
            err = "bind " + socketPath_ + ": " + errnoString(errno);
            return false;
        }
        socketCreated_ = true;
        if (::chmod(socketPath_.c_str(), 0600) != 0) {
            logError("chmod " + socketPath_ + ": " + errnoString(errno));
        }
        if (::listen(listenFd_, 16) != 0) {
            err = "listen: " + errnoString(errno);
            return false;
        }
        return true;
    }

    void wake() {
        const char byte = 1;
        ssize_t ignored = ::write(wake_[1], &byte, 1);
        (void)ignored;
    }

    void drainWake() {
        char buffer[256];
        while (::read(wake_[0], buffer, sizeof(buffer)) > 0) {
        }
    }

    void acceptClients() {
        for (;;) {
            const int fd =
                ::accept4(listenFd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                logError(std::string("accept: ") + errnoString(errno));
                break;
            }
            auto client = std::make_unique<Client>();
            client->cid = nextClientId_++;
            client->fd = fd;
            logDebug("client connected cid=" + std::to_string(client->cid));
            clients_.push_back(std::move(client));
        }
    }

    Client *findClient(uint64_t cid) {
        for (auto &client : clients_) {
            if (client->cid == cid && client->fd >= 0) {
                return client.get();
            }
        }
        return nullptr;
    }

    void disconnectClient(Client *client) {
        if (client->fd < 0) {
            return;
        }
        logDebug("disconnecting cid=" + std::to_string(client->cid));
        if (recording_.active && recording_.clientId == client->cid) {
            recorder_.cancel();
            recording_ = {};
            partialTimerActive_ = false;
        }
        for (auto &job : jobs_) {
            if (job->clientId == client->cid) {
                job->cancelled.store(true);
            }
        }
        ::close(client->fd);
        client->fd = -1;
        client->out.clear();
        client->outSent = 0;
    }

    bool processLines(Client *client) {
        while (auto line = client->input.nextLine()) {
            if (client->fd < 0) {
                return false;
            }
            auto message = koe::parse(*line);
            if (!message) {
                logError("cid=" + std::to_string(client->cid) +
                         " sent malformed line, disconnecting");
                disconnectClient(client);
                return false;
            }
            handleMessage(*message, client);
        }
        return client->fd >= 0;
    }

    void readClient(Client *client) {
        char buffer[4096];
        for (;;) {
            const ssize_t n = ::read(client->fd, buffer, sizeof(buffer));
            if (n > 0) {
                client->input.append(buffer, static_cast<size_t>(n));
                if (client->input.overflowed()) {
                    logError("cid=" + std::to_string(client->cid) +
                             " line too long, disconnecting");
                    disconnectClient(client);
                    return;
                }
                if (!processLines(client)) {
                    return;
                }
                continue;
            }
            if (n == 0) {
                client->readClosed = true;
                logDebug("cid=" + std::to_string(client->cid) + " closed input");
                processLines(client);
                if (client->fd >= 0 && client->out.empty()) {
                    disconnectClient(client);
                }
                return;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                processLines(client);
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            logError(std::string("read: ") + errnoString(errno));
            disconnectClient(client);
            return;
        }
    }

    void flushClient(Client *client) {
        while (client->outSent < client->out.size()) {
            const ssize_t n =
                ::write(client->fd, client->out.data() + client->outSent,
                        client->out.size() - client->outSent);
            if (n > 0) {
                client->outSent += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            logError(std::string("write: ") + errnoString(errno));
            disconnectClient(client);
            return;
        }
        client->out.clear();
        client->outSent = 0;
        if (client->readClosed) {
            disconnectClient(client);
        }
    }

    void send(Client *client, const koe::Message &message) {
        if (!client || client->fd < 0) {
            return;
        }
        client->out += koe::format(message);
        if (client->out.size() > kMaxClientOut) {
            logError("cid=" + std::to_string(client->cid) +
                     " output buffer overflow, disconnecting");
            disconnectClient(client);
        }
    }

    void sendText(Client *client, uint64_t id, const std::string &text) {
        koe::Message message;
        message.type = koe::Message::Type::Text;
        message.id = id;
        message.payload = text;
        send(client, message);
        logDebug("-> TEXT " + std::to_string(id) + " [" +
                 std::to_string(text.size()) + " chars]");
    }

    void sendError(Client *client, uint64_t id, const std::string &text) {
        koe::Message message;
        message.type = koe::Message::Type::Error;
        message.id = id;
        message.payload = text;
        send(client, message);
        logDebug("-> ERROR " + std::to_string(id) + " " + text);
    }

    void sendPartial(Client *client, uint64_t id, const std::string &text) {
        koe::Message message;
        message.type = koe::Message::Type::Partial;
        message.id = id;
        message.payload = text;
        send(client, message);
        logDebug("-> PARTIAL " + std::to_string(id) + " [" +
                 std::to_string(text.size()) + " chars]");
    }

    void handleMessage(const koe::Message &message, Client *client) {
        switch (message.type) {
        case koe::Message::Type::Start:
            handleStart(message, client);
            break;
        case koe::Message::Type::Stop:
            handleStop(message, client);
            break;
        case koe::Message::Type::Cancel:
            handleCancel(message, client);
            break;
        case koe::Message::Type::Text:
        case koe::Message::Type::Error:
        case koe::Message::Type::Partial:
            logError("cid=" + std::to_string(client->cid) +
                     " sent server-only message, disconnecting");
            disconnectClient(client);
            break;
        }
    }

    void handleStart(const koe::Message &message, Client *client) {
        if (recording_.active) {
            logDebug("cancelling active recording id=" +
                     std::to_string(recording_.id) + " for new START");
            cancelPartialJobs(recording_.id);
            recorder_.cancel();
            recording_ = {};
            partialTimerActive_ = false;
        }
        if (recorderUnavailable_) {
            sendError(client, message.id, recorderError_);
            return;
        }
        std::string error;
        if (!recorder_.start(error)) {
            sendError(client, message.id, error);
            return;
        }
        recording_.active = true;
        recording_.id = message.id;
        recording_.clientId = client->cid;
        recording_.lang = message.payload;
        if (config_.profile.partialIntervalMs > 0) {
            partialTimerActive_ = true;
            nextPartialAt_ =
                std::chrono::steady_clock::now() +
                std::chrono::milliseconds(config_.profile.partialIntervalMs);
        } else {
            partialTimerActive_ = false;
        }
        logDebug("START id=" + std::to_string(message.id) +
                 " lang=" + message.payload);
    }

    void handleStop(const koe::Message &message, Client *client) {
        if (!recording_.active || recording_.id != message.id ||
            recording_.clientId != client->cid) {
            sendError(client, message.id, "not recording");
            return;
        }

        std::vector<float> pcm = recorder_.stop();
        const uint64_t id = recording_.id;
        const uint64_t clientId = recording_.clientId;
        std::string lang = recording_.lang;
        recording_ = {};
        partialTimerActive_ = false;
        cancelPartialJobs(id);

        if (recorder_.takeCapped()) {
            logInfo("id=" + std::to_string(id) +
                    " recording reached max_record_sec, truncated");
        }

        if (pcm.empty()) {
            logError("STOP id=" + std::to_string(id) +
                     " no audio captured, check microphone");
            sendError(client, id, "no audio captured (check microphone)");
            return;
        }

        if (pcm.size() < kMinClipSamples) {
            logDebug("STOP id=" + std::to_string(id) + " clip too short (" +
                     formatSeconds(pcm.size()) + "s), empty text");
            sendText(client, id, "");
            return;
        }

        const double rms = clipRms(pcm);
        logDebug("STOP id=" + std::to_string(id) + " rms=" + formatRms(rms));

        if (config_.minRms > 0.0f && rms < static_cast<double>(config_.minRms)) {
            logDebug("STOP id=" + std::to_string(id) +
                     " below min_rms, empty text");
            sendText(client, id, "");
            return;
        }

        enqueueJob(id, clientId, std::move(lang), std::move(pcm), false);
    }

    void handleCancel(const koe::Message &message, Client *client) {
        if (recording_.active && recording_.id == message.id &&
            recording_.clientId == client->cid) {
            cancelPartialJobs(recording_.id);
            recorder_.cancel();
            recording_ = {};
            partialTimerActive_ = false;
            return;
        }
        for (auto &job : jobs_) {
            if (job->id == message.id && job->clientId == client->cid) {
                job->cancelled.store(true);
            }
        }
    }

    void handleRecorderError() {
        std::string error;
        if (!recorder_.takeError(error)) {
            return;
        }
        logError("audio stream error: " + error);
        cancelPartialJobs(recording_.id);
        recorder_.cancel();
        partialTimerActive_ = false;
        if (!recording_.active) {
            return;
        }
        const uint64_t id = recording_.id;
        Client *owner = findClient(recording_.clientId);
        recording_ = {};
        sendError(owner, id, error);
    }

    void enqueueJob(uint64_t id, uint64_t clientId, std::string lang,
                    std::vector<float> pcm, bool partial) {
        auto job = std::make_shared<Job>();
        job->id = id;
        job->clientId = clientId;
        job->lang = std::move(lang);
        job->pcm = std::move(pcm);
        job->partial = partial;
        jobs_.push_back(job);
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            jobQueue_.push_back(job);
        }
        queueCv_.notify_one();
        logDebug(std::string(partial ? "queued partial" : "queued job") +
                 " id=" + std::to_string(id) + " audio=" +
                 formatSeconds(job->pcm.size()) + "s");
    }

    void cancelPartialJobs(uint64_t id) {
        for (auto &job : jobs_) {
            if (job->partial && job->id == id && !job->cancelled.load()) {
                job->cancelled.store(true);
                logDebug("cancelled partial job id=" + std::to_string(id));
            }
        }
    }

    bool partialInFlight(uint64_t id) const {
        for (const auto &job : jobs_) {
            if (job->partial && job->id == id) {
                return true;
            }
        }
        return false;
    }

    void maybeSchedulePartial() {
        if (!recording_.active || config_.profile.partialIntervalMs <= 0) {
            partialTimerActive_ = false;
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < nextPartialAt_) {
            return;
        }
        nextPartialAt_ =
            now + std::chrono::milliseconds(config_.profile.partialIntervalMs);

        const uint64_t id = recording_.id;
        if (partialInFlight(id)) {
            logDebug("partial id=" + std::to_string(id) +
                     " skipped: previous pass still in flight");
            return;
        }

        std::vector<float> pcm = recorder_.snapshot();
        if (pcm.size() < kPartialMinSamples) {
            logDebug("partial id=" + std::to_string(id) +
                     " skipped: audio too short (" + formatSeconds(pcm.size()) +
                     "s)");
            return;
        }

        const double rms = clipRms(pcm);
        if (config_.minRms > 0.0f &&
            rms < static_cast<double>(config_.minRms)) {
            logDebug("partial id=" + std::to_string(id) +
                     " skipped: below min_rms (rms=" + formatRms(rms) + ")");
            return;
        }

        enqueueJob(id, recording_.clientId, recording_.lang, std::move(pcm),
                   true);
    }

    int pollTimeoutMs() const {
        if (!partialTimerActive_ || !recording_.active) {
            return -1;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextPartialAt_) {
            return 0;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(nextPartialAt_ -
                                                                  now)
                .count();
        return static_cast<int>(std::min<int64_t>(remaining, 3600000));
    }

    void drainResults() {
        std::deque<Result> results;
        {
            std::lock_guard<std::mutex> lock(resultMutex_);
            results.swap(results_);
        }
        for (auto &result : results) {
            jobs_.erase(std::remove(jobs_.begin(), jobs_.end(), result.job),
                        jobs_.end());
            if (result.cancelled || result.job->cancelled.load()) {
                continue;
            }
            Client *client = findClient(result.job->clientId);
            if (!client) {
                continue;
            }
            if (result.job->partial) {
                if (!result.ok) {
                    logDebug("partial id=" + std::to_string(result.job->id) +
                             " error: " + result.error);
                    continue;
                }
                if (result.text.empty()) {
                    continue;
                }
                if (!recording_.active ||
                    recording_.id != result.job->id ||
                    recording_.clientId != result.job->clientId) {
                    logDebug("partial id=" + std::to_string(result.job->id) +
                             " dropped: recording no longer active");
                    continue;
                }
                sendPartial(client, result.job->id, result.text);
                continue;
            }
            if (result.ok) {
                sendText(client, result.job->id, result.text);
            } else {
                sendError(client, result.job->id, result.error);
            }
        }
    }

    void pushResult(Result result) {
        {
            std::lock_guard<std::mutex> lock(resultMutex_);
            results_.push_back(std::move(result));
        }
        wake();
    }

    void workerLoop() {
        koe::Transcriber transcriber(config_.profile);
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueCv_.wait(lock, [this]() {
                    return stopWorker_ || !jobQueue_.empty();
                });
                if (stopWorker_) {
                    return;
                }
                job = jobQueue_.front();
                jobQueue_.pop_front();
            }

            if (job->cancelled.load()) {
                Result message;
                message.job = job;
                message.cancelled = true;
                pushResult(std::move(message));
                continue;
            }

            const auto start = std::chrono::steady_clock::now();
            koe::TranscribeResult result =
                transcriber.run(job->pcm, job->lang, stopFlag_);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();

            if (stopFlag_.load()) {
                return;
            }

            if (job->partial) {
                logDebug("partial id=" + std::to_string(job->id) + " audio=" +
                         formatSeconds(job->pcm.size()) + "s req_ms=" +
                         std::to_string(elapsed) + " chars=" +
                         std::to_string(result.text.size()));
            } else {
                logInfo("job id=" + std::to_string(job->id) + " audio=" +
                        formatSeconds(job->pcm.size()) + "s req_ms=" +
                        std::to_string(elapsed) + " chars=" +
                        std::to_string(result.text.size()));
            }

            Result message;
            message.job = job;
            message.ok = result.ok;
            message.text = std::move(result.text);
            message.error = std::move(result.error);
            pushResult(std::move(message));
        }
    }

    koe::Config config_;
    std::string socketPath_;
    int listenFd_ = -1;
    int sigFd_ = -1;
    int wake_[2] = {-1, -1};
    uint64_t nextClientId_ = 1;

    std::vector<std::unique_ptr<Client>> clients_;
    Recording recording_;
    std::vector<std::shared_ptr<Job>> jobs_;
    bool partialTimerActive_ = false;
    std::chrono::steady_clock::time_point nextPartialAt_;

    koe::Recorder recorder_;
    bool recorderUnavailable_ = false;
    std::string recorderError_;

    std::thread worker_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<std::shared_ptr<Job>> jobQueue_;
    bool stopWorker_ = false;
    std::atomic<bool> stopFlag_{false};

    std::mutex resultMutex_;
    std::deque<Result> results_;

    bool cleanedUp_ = false;
    bool socketCreated_ = false;
};

void usage(const char *program) {
    std::cerr << "usage: " << program
              << " [--config <path>] [--socket <path>] [-v]\n";
}

} // namespace

int main(int argc, char **argv) {
    std::string configPath;
    std::string socketOverride;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-v") {
            g_verbose = true;
        } else if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "--socket" && i + 1 < argc) {
            socketOverride = argv[++i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) {
        logError(std::string("sigprocmask: ") + errnoString(errno));
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    koe::Config config;
    bool missing = false;
    std::string error;
    if (!koe::loadConfig(configPath, config, missing, error)) {
        logError(error);
        return 1;
    }
    if (missing) {
        logInfo("no config file found, using built-in default profile 'local'");
    } else {
        logInfo("using config profile '" + config.active + "'");
    }

    std::string socketPath = socketOverride;
    if (socketPath.empty()) {
        const char *runtimeDir = std::getenv("XDG_RUNTIME_DIR");
        if (!runtimeDir || !*runtimeDir) {
            logError("XDG_RUNTIME_DIR is not set; pass --socket <path>");
            return 1;
        }
        socketPath = std::string(runtimeDir) + "/koe.sock";
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    Daemon daemon(std::move(config), std::move(socketPath));
    if (!daemon.init(error)) {
        logError(error);
        curl_global_cleanup();
        return 1;
    }

    logInfo("listening");
    const int status = daemon.run();
    daemon.cleanup();
    curl_global_cleanup();
    return status;
}
