#ifndef KOE_ADDON_KOE_H
#define KOE_ADDON_KOE_H

#include <fcitx-config/configuration.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/option.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/handlertable.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/trackableobject.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/instance.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/protocol.h"

namespace koe {

using namespace fcitx;

FCITX_CONFIGURATION(
    KoeConfig,
    KeyListOption hotkey{
        this,
        "Hotkey",
        _("Hotkey"),
        {Key("Control_R")},
        KeyListConstrain(KeyConstrainFlags{KeyConstrainFlag::AllowModifierOnly,
                                           KeyConstrainFlag::AllowModifierLess})};
    Option<std::string> language{this, "Language", _("Language"), "auto"};);

class Koe final : public AddonInstance {
public:
    explicit Koe(Instance *instance);
    ~Koe() override;

    const Configuration *getConfig() const override { return &config_; }
    void setConfig(const RawConfig &config) override;
    void reloadConfig() override;

private:
    struct Recording {
        uint64_t id = 0;
        TrackableObjectReference<InputContext> ic;
        Key key;
        bool ownsPreedit = false;
    };

    struct Pending {
        TrackableObjectReference<InputContext> ic;
        bool ownsPreedit = false;
    };

    bool ensureConnected();
    void disconnect(bool notifyUser);
    bool sendMessage(const Message &message);
    bool onSocketEvent(EventSourceIO *source, int fd, IOEventFlags flags);
    void handleLine(const std::string &line);
    void handleText(const Message &message);
    void handleError(const Message &message);
    void handlePartial(const Message &message);

    void onKeyEvent(KeyEvent &event);
    void onFocusOut(InputContext *ic);
    void cancelRecording();

    void setAux(InputContext *ic, const std::string &text);
    void clearAux(InputContext *ic);
    void showPreedit(InputContext *ic, const std::string &text);
    void clearPreedit(InputContext *ic);
    void notify(const std::string &summary, const std::string &body);

    std::string socketPath() const;
    std::string languageCode() const;
    bool isHotkeyRelease(const Key &released, const Key &pressed) const;
    bool isConfiguredHotkeyRelease(const Key &released) const;

    Instance *instance_;

    KoeConfig config_;
    uint64_t nextId_ = 1;
    std::optional<Recording> recording_;
    std::unordered_map<uint64_t, Pending> pending_;

    int fd_ = -1;
    std::unique_ptr<EventSourceIO> ioEvent_;
    LineBuffer buffer_;
    std::vector<std::unique_ptr<HandlerTableEntry<EventHandler>>> eventWatchers_;

    FCITX_ADDON_DEPENDENCY_LOADER(notifications, instance_->addonManager());
};

class KoeFactory : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override;
};

} // namespace koe

#endif
