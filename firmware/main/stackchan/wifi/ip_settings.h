#pragma once
// Per-SSID IPv4 settings. Zero-initialized means DHCP, preserving old Wi-Fi
// configurations. Inspired by the S3-stackchan parser; no heap allocations.
#include <cstdint>
#include <cstring>

namespace stackchan::wifi {
struct IpSettings {
    uint8_t use_static = 0; // 0=DHCP, 1=static; safe to deserialize from NVS
    uint8_t ip[4]{}, subnet[4]{}, gateway[4]{}, dns1[4]{}, dns2[4]{};
};

inline bool ParseIPv4(const char* text, uint8_t (&out)[4]) {
    if (!text) return false;
    uint8_t parsed[4]{};
    for (unsigned part = 0; part < 4; ++part) {
        unsigned value = 0, digits = 0;
        while (*text >= '0' && *text <= '9') {
            value = value * 10 + static_cast<unsigned>(*text++ - '0');
            if (++digits > 3 || value > 255) return false;
        }
        if (!digits || *text != (part == 3 ? '\0' : '.')) return false;
        parsed[part] = static_cast<uint8_t>(value);
        if (part != 3) ++text;
    }
    std::memcpy(out, parsed, sizeof(parsed));
    return true;
}

inline uint32_t IPv4Number(const uint8_t (&addr)[4]) {
    return uint32_t(addr[0]) << 24 | uint32_t(addr[1]) << 16 |
           uint32_t(addr[2]) << 8 | uint32_t(addr[3]);
}

inline bool IsUnicastIPv4(const uint8_t (&addr)[4]) {
    return addr[0] != 0 && addr[0] != 127 && addr[0] < 224 &&
           IPv4Number(addr) != 0xffffffffU;
}

inline bool ValidIpSettings(const IpSettings& s) {
    if (s.use_static > 1) return false;
    if (!s.use_static) return true;
    const uint32_t ip = IPv4Number(s.ip);
    const uint32_t mask = IPv4Number(s.subnet);
    const uint32_t gateway = IPv4Number(s.gateway);
    const uint32_t hosts = ~mask;
    return IsUnicastIPv4(s.ip) && IsUnicastIPv4(s.gateway) &&
           IsUnicastIPv4(s.dns1) &&
           (!IPv4Number(s.dns2) || IsUnicastIPv4(s.dns2)) &&
           mask && !(hosts & (hosts + 1U)) &&
           (ip & mask) == (gateway & mask) && ip != gateway &&
           (hosts <= 1U || ((ip & hosts) != 0 && (ip & hosts) != hosts &&
                           (gateway & hosts) != 0 && (gateway & hosts) != hosts));
}
}  // namespace stackchan::wifi
