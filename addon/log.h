#ifndef KOE_ADDON_LOG_H
#define KOE_ADDON_LOG_H

#include <fcitx-utils/log.h>

namespace koe {

FCITX_DECLARE_LOG_CATEGORY(koe_log);

} // namespace koe

#define KOE_DEBUG() FCITX_LOGC(::koe::koe_log, Debug)
#define KOE_INFO() FCITX_LOGC(::koe::koe_log, Info)
#define KOE_WARN() FCITX_LOGC(::koe::koe_log, Warn)
#define KOE_ERROR() FCITX_LOGC(::koe::koe_log, Error)

#endif
