#pragma once
#include <cstdint>

namespace stackchan::config::server_boot {
enum class Protocol { Legacy, Websocket, Mqtt };
struct Plan {
    Protocol protocol = Protocol::Legacy;
    bool seed_url = false;
    bool seed_version = false;
};

// During migration, ambiguous or unreadable configurations retain the old
// provisioning path. Never guess which of two stored transports was active.
constexpr Plan PlanBoot(bool readable, bool has_ws, bool canonical_ws,
                        bool has_mqtt, bool convertible_legacy, int32_t version) {
    if (!readable || (has_ws && has_mqtt)) return {};
    if (has_mqtt) return {Protocol::Mqtt, false, false};
    if (version < 0 || version > 3) return {};
    if (has_ws && !canonical_ws) return {};
    if (!has_ws && !convertible_legacy) return {};
    return {Protocol::Websocket, !has_ws, version == 0};
}

// Called during HAL initialization, after possible SD restore, before any
// network service can change the settings. No legacy keys are erased.
void PrepareStoredProtocol();
// Read-only selection; requires all planned migration writes to be complete.
Protocol ReadDirectProtocol();
} // namespace stackchan::config::server_boot
