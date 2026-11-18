#pragma once
#include "ip_settings.h"
#include <string_view>

namespace stackchan::wifi {
// Saved separately from SsidManager's existing wifi namespace. A missing
// per-SSID value always means DHCP, including devices upgraded from old builds.
bool LoadIpSettings(std::string_view ssid, IpSettings& out);
bool SaveIpSettings(std::string_view ssid, const IpSettings& settings);
bool RemoveIpSettings(std::string_view ssid);
}
