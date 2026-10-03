#include "koe.h"
#include "log.h"

#include <fcitx/inputpanel.h>
#include <fcitx/text.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <notifications_public.h>

namespace koe {

FCITX_DEFINE_LOG_CATEGORY(koe_log, "koe");

namespace {

constexpr int32_t kNotifyTimeout = 5000;

} // namespace

Koe::Koe(Instance *instance) : instance_(instance) {
    reloadConfig();

    eventWatchers_.emplace_back(instance_->watchEvent(
        EventType::InputContextKeyEvent, EventWatcherPhase::PreInputMethod,
        [this](Event &event) {
            onKeyEvent(static_cast<KeyEvent &>(event));
        }));
    eventWatchers_.emplace_back(instance_->watchEvent(
        EventType::InputContextFocusOut, EventWatcherPhase::InputMethod,
        [this](Event &event) {
            onFocusOut(static_cast<FocusOutEvent &>(event).inputContext());
        }));

    KOE_INFO() << "koe loaded";
}

Koe::~Koe() {
    eventWatchers_.clear();
    ioEvent_.reset();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void Koe::reloadConfig() { readAsIni(config_, "conf/koe.conf"); }

void Koe::setConfig(const RawConfig &config) {
    config_.load(config, true);
    safeSaveAsIni(config_, "conf/koe.conf");
}

std::string Koe::socketPath() const {
    const char *runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    if (!runtimeDir || !*runtimeDir) {
        return {};
    }
    return std::string(runtimeDir) + "/koe.sock";
}

std::string Koe::languageCode() const {
    const std::string lang = config_.language.value();
    if (lang.empty() || lang.size() > 16 ||
        lang.find(' ') != std::string::npos) {
        return "auto";
    }
    return lang;
}

bool Koe::isHotkeyRelease(const Key &released, const Key &pressed) const {
    return released.isReleaseOfModifier(pressed) ||
           released.sym() == pressed.sym();
}

bool Koe::isConfiguredHotkeyRelease(const Key &released) const {
    for (const auto &configured : config_.hotkey.value()) {
        if (isHotkeyRelease(released, configured)) {
            return true;
        }
    }
    return false;
}

void Koe::notify(const std::string &summary, const std::string &body) {
    auto *addon = notifications();
    if (!addon) {
        return;
    }
    addon->call<INotifications::showTip>("koe", "Koe", "audio-input-microphone",
                                         summary, body, kNotifyTimeout);
}

void Koe::setAux(InputContext *ic, const std::string &text) {
    if (!ic) {
        return;
    }
    ic->inputPanel().setAuxUp(Text(text));
    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
}

void Koe::clearAux(InputContext *ic) {
    if (!ic) {
        return;
    }
    ic->inputPanel().setAuxUp(Text());
    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
}

void Koe::showPreedit(InputContext *ic, const std::string &text) {
    if (!ic) {
        return;
    }
    Text preedit(text, TextFormatFlag::Underline);
    preedit.setCursor(static_cast<int>(text.size()));
    if (ic->capabilityFlags().test(CapabilityFlag::Preedit)) {
        ic->inputPanel().setClientPreedit(preedit);
        ic->updatePreedit();
    } else {
        ic->inputPanel().setPreedit(preedit);
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
    }
}

void Koe::clearPreedit(InputContext *ic) {
    if (!ic) {
        return;
    }
    if (ic->capabilityFlags().test(CapabilityFlag::Preedit)) {
        ic->inputPanel().setClientPreedit(Text());
        ic->updatePreedit();
    } else {
        ic->inputPanel().setPreedit(Text());
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
    }
}

bool Koe::ensureConnected() {
    if (fd_ >= 0) {
        return true;
    }

    const std::string path = socketPath();
    if (path.empty()) {
        KOE_WARN() << "XDG_RUNTIME_DIR is not set";
        notify("Voice input", "koe daemon is not running");
        return false;
    }

    const int fd =
        ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        KOE_WARN() << "socket: " << std::strerror(errno);
        notify("Voice input", "koe daemon is not running");
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        KOE_ERROR() << "socket path too long: " << path;
        notify("Voice input", "koe socket path is too long");
        return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        const int error = errno;
        ::close(fd);
        KOE_WARN() << "connect " << path << ": " << std::strerror(error);
        notify("Voice input", "koe daemon is not running");
        return false;
    }

    fd_ = fd;
    buffer_.clear();
    ioEvent_ = instance_->eventLoop().addIOEvent(
        fd_,
        IOEventFlags{IOEventFlag::In, IOEventFlag::Err, IOEventFlag::Hup},
        [this](EventSourceIO *source, int eventFd, IOEventFlags flags) {
            return onSocketEvent(source, eventFd, flags);
        });
    KOE_INFO() << "connected to " << path;
    return true;
}

void Koe::disconnect(bool notifyUser) {
    if (fd_ < 0) {
        return;
    }

    const bool hadActivity = recording_.has_value() || !pending_.empty();

    ::close(fd_);
    fd_ = -1;
    ioEvent_.reset();
    buffer_.clear();

    if (recording_) {
        if (recording_->ownsPreedit) {
            clearPreedit(recording_->ic.get());
        }
        clearAux(recording_->ic.get());
        recording_.reset();
    }
    for (auto &entry : pending_) {
        if (entry.second.ownsPreedit) {
            clearPreedit(entry.second.ic.get());
        }
        clearAux(entry.second.ic.get());
    }
    pending_.clear();

    KOE_INFO() << "disconnected from daemon";
    if (notifyUser && hadActivity) {
        notify("Voice input", "koe daemon disconnected");
    }
}

bool Koe::sendMessage(const Message &message) {
    if (fd_ < 0) {
        return false;
    }

    const std::string line = format(message);
    ssize_t written = -1;
    do {
        written = ::send(fd_, line.data(), line.size(), MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);

    if (written != static_cast<ssize_t>(line.size())) {
        KOE_WARN() << "short write to daemon, disconnecting";
        disconnect(true);
        return false;
    }
    return true;
}

bool Koe::onSocketEvent(EventSourceIO *, int, IOEventFlags flags) {
    if (flags.test(IOEventFlag::Err) || flags.test(IOEventFlag::Hup)) {
        disconnect(true);
        return false;
    }
    if (!flags.test(IOEventFlag::In)) {
        return true;
    }

    char chunk[4096];
    for (;;) {
        const ssize_t n = ::read(fd_, chunk, sizeof(chunk));
        if (n > 0) {
            buffer_.append(chunk, static_cast<size_t>(n));
            if (buffer_.overflowed()) {
                KOE_ERROR() << "daemon sent an overlong line, disconnecting";
                disconnect(true);
                return false;
            }
            while (auto line = buffer_.nextLine()) {
                handleLine(*line);
                if (fd_ < 0) {
                    return false;
                }
            }
            continue;
        }
        if (n == 0) {
            disconnect(true);
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        if (errno == EINTR) {
            continue;
        }
        KOE_WARN() << "read: " << std::strerror(errno);
        disconnect(true);
        return false;
    }
}

void Koe::handleLine(const std::string &line) {
    auto message = parse(line);
    if (!message) {
        KOE_WARN() << "unparsable line from daemon, disconnecting";
        disconnect(true);
        return;
    }
    switch (message->type) {
    case Message::Type::Text:
        handleText(*message);
        break;
    case Message::Type::Error:
        handleError(*message);
        break;
    case Message::Type::Partial:
        handlePartial(*message);
        break;
    default:
        KOE_WARN() << "unexpected message from daemon, disconnecting";
        disconnect(true);
        break;
    }
}

void Koe::handleText(const Message &message) {
    auto it = pending_.find(message.id);
    if (it == pending_.end()) {
        return;
    }
    Pending entry = std::move(it->second);
    pending_.erase(it);

    InputContext *ic = entry.ic.get();
    if (entry.ownsPreedit) {
        clearPreedit(ic);
    }
    clearAux(ic);
    if (message.payload.empty()) {
        return;
    }
    if (ic && ic->hasFocus()) {
        ic->commitString(message.payload);
    } else {
        notify("Voice input (not typed, focus changed)", message.payload);
    }
}

void Koe::handlePartial(const Message &message) {
    if (!recording_ || recording_->id != message.id) {
        return;
    }
    if (message.payload.empty()) {
        return;
    }
    InputContext *ic = recording_->ic.get();
    if (!ic || !ic->hasFocus()) {
        return;
    }
    showPreedit(ic, message.payload);
    recording_->ownsPreedit = true;
}

void Koe::handleError(const Message &message) {
    InputContext *ic = nullptr;
    bool ownsPreedit = false;
    auto it = pending_.find(message.id);
    if (it != pending_.end()) {
        ic = it->second.ic.get();
        ownsPreedit = it->second.ownsPreedit;
        pending_.erase(it);
    }
    if (recording_ && recording_->id == message.id) {
        if (!ic) {
            ic = recording_->ic.get();
        }
        ownsPreedit = ownsPreedit || recording_->ownsPreedit;
        recording_.reset();
    }
    if (ownsPreedit) {
        clearPreedit(ic);
    }
    clearAux(ic);

    const std::string body =
        message.payload.empty() ? "unknown error" : message.payload;
    notify("Voice input error", body);
}

void Koe::cancelRecording() {
    if (!recording_) {
        return;
    }

    Message message;
    message.type = Message::Type::Cancel;
    message.id = recording_->id;
    sendMessage(message);

    if (recording_->ownsPreedit) {
        clearPreedit(recording_->ic.get());
    }
    clearAux(recording_->ic.get());
    recording_.reset();
}

void Koe::onFocusOut(InputContext *ic) {
    if (recording_ && recording_->ic.get() == ic) {
        Message message;
        message.type = Message::Type::Cancel;
        message.id = recording_->id;
        sendMessage(message);

        if (recording_->ownsPreedit) {
            clearPreedit(ic);
        }
        clearAux(ic);
        recording_.reset();
        return;
    }

    for (auto &entry : pending_) {
        if (entry.second.ic.get() == ic && entry.second.ownsPreedit) {
            clearPreedit(ic);
            entry.second.ownsPreedit = false;
        }
    }
}

void Koe::onKeyEvent(KeyEvent &event) {
    if (event.isVirtual()) {
        return;
    }

    InputContext *ic = event.inputContext();
    if (!ic) {
        return;
    }

    if (!event.isRelease()) {
        if (!event.key().checkKeyList(config_.hotkey.value())) {
            if (recording_ && !event.key().isModifier()) {
                cancelRecording();
            }
            return;
        }
        event.filterAndAccept();
        if (recording_) {
            return;
        }
        if (!ensureConnected()) {
            return;
        }

        const uint64_t id = nextId_++;
        Recording recording;
        recording.id = id;
        recording.ic = ic->watch();
        recording.key = event.key();

        Message message;
        message.type = Message::Type::Start;
        message.id = id;
        message.payload = languageCode();
        if (!sendMessage(message)) {
            return;
        }

        recording_ = std::move(recording);
        setAux(ic, "\xF0\x9F\x8E\x99 Recording...");
        return;
    }

    if (recording_) {
        const Key &pressed = recording_->key;
        if (isHotkeyRelease(event.key(), pressed)) {
            event.filterAndAccept();

            const uint64_t id = recording_->id;
            InputContext *ic = recording_->ic.get();
            Pending entry;
            entry.ic = recording_->ic;
            entry.ownsPreedit = recording_->ownsPreedit;
            recording_.reset();
            pending_.emplace(id, std::move(entry));

            Message message;
            message.type = Message::Type::Stop;
            message.id = id;
            if (sendMessage(message)) {
                setAux(ic, "\xE2\x8F\xB3 Transcribing...");
            }
            return;
        }
    }

    if (isConfiguredHotkeyRelease(event.key())) {
        event.filterAndAccept();
    }
}

AddonInstance *KoeFactory::create(AddonManager *manager) {
    return new Koe(manager->instance());
}

} // namespace koe

FCITX_ADDON_FACTORY_V2(koe, koe::KoeFactory)
