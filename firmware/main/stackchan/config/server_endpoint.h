#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace stackchan::config::server_endpoint {
inline constexpr std::size_t kMaxServerBase = 192;
inline constexpr std::size_t kMaxEndpoint = 256;
inline constexpr const char* kOtaSuffix = "/xiaozhi/ota/";
inline constexpr const char* kWebsocketSuffix = "/xiaozhi/ws";

// Preserve the existing SETUP Web URL contract during the staged migration.
inline bool ValidServerBase(const char* value) {
    if (!value) return false;
    const auto length = std::strlen(value);
    if (length < 8 || length > kMaxServerBase) return false;
    const char* authority = nullptr;
    if (std::strncmp(value, "http://", 7) == 0) authority = value + 7;
    else if (std::strncmp(value, "https://", 8) == 0) authority = value + 8;
    else return false;
    if (!*authority || *authority == '/') return false;
    for (const auto* p = reinterpret_cast<const unsigned char*>(authority); *p; ++p) {
        if (*p <= 0x20 || *p == 0x7f || *p == '"' || *p == '\\' ||
            *p == '?' || *p == '#' || *p == '@') return false;
        if (*p == '/' && p[1] != '\0') return false;
    }
    return true;
}

// No heap allocation; both caller-owned buffers must fit before success.
inline bool BuildServerEndpoints(const char* base, char* ota, std::size_t ota_capacity,
                                 char* websocket, std::size_t websocket_capacity) {
    if (!ota || !ota_capacity || !websocket || !websocket_capacity || !ValidServerBase(base)) return false;
    char normalized[kMaxServerBase + 1]{};
    std::snprintf(normalized, sizeof(normalized), "%s", base);
    const auto length = std::strlen(normalized);
    if (normalized[length - 1] == '/') normalized[length - 1] = '\0';
    const char* authority = std::strstr(normalized, "://") + 3;
    const char* ws_scheme = std::strncmp(normalized, "https://", 8) == 0 ? "wss" : "ws";
    const int ota_length = std::snprintf(ota, ota_capacity, "%s%s", normalized, kOtaSuffix);
    const int ws_length = std::snprintf(websocket, websocket_capacity, "%s://%s%s",
                                        ws_scheme, authority, kWebsocketSuffix);
    return ota_length > 0 && static_cast<std::size_t>(ota_length) < ota_capacity &&
           ws_length > 0 && static_cast<std::size_t>(ws_length) < websocket_capacity;
}

inline bool ServerBaseFromOta(const char* ota, char* output, std::size_t capacity) {
    if (!ota || !output || !capacity) return false;
    const auto length = std::strlen(ota);
    const auto suffix_length = std::strlen(kOtaSuffix);
    if (length <= suffix_length || std::strcmp(ota + length - suffix_length, kOtaSuffix) != 0 ||
        length - suffix_length >= capacity) return false;
    std::memcpy(output, ota, length - suffix_length);
    output[length - suffix_length] = '\0';
    return ValidServerBase(output);
}

// Reverse only the canonical WebSocket endpoint. Do not rewrite arbitrary
// stored URLs; the caller must distinguish a missing key from a custom URL.
inline bool ServerBaseFromWebsocket(const char* ws, char* output, std::size_t capacity) {
    if (!ws || !output || !capacity) return false;
    const char* scheme = nullptr;
    const char* authority = nullptr;
    if (std::strncmp(ws, "ws://", 5) == 0) { scheme = "http://"; authority = ws + 5; }
    else if (std::strncmp(ws, "wss://", 6) == 0) { scheme = "https://"; authority = ws + 6; }
    else return false;
    const auto length = std::strlen(authority);
    const auto suffix_length = std::strlen(kWebsocketSuffix);
    if (length <= suffix_length || std::strcmp(authority + length - suffix_length, kWebsocketSuffix) != 0) return false;
    const auto host_length = length - suffix_length;
    const auto scheme_length = std::strlen(scheme);
    if (host_length > kMaxServerBase - scheme_length || scheme_length + host_length >= capacity) return false;
    // Reject paths, including a duplicate slash immediately before the suffix.
    if (std::memchr(authority, '/', host_length)) return false;
    std::memcpy(output, scheme, scheme_length);
    std::memcpy(output + scheme_length, authority, host_length);
    output[scheme_length + host_length] = '\0';
    return ValidServerBase(output);
}
} // namespace stackchan::config::server_endpoint
