#include "stackchan/config/server_endpoint.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

using namespace stackchan::config::server_endpoint;
static void check(bool result, const char* label) {
    if (!result) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
int main() {
    char ota[kMaxEndpoint]{}, ws[kMaxEndpoint]{}, base[kMaxServerBase + 1]{};
    struct Case { const char* input; const char* normalized; const char* ota; const char* ws; };
    const Case cases[] = {
        {"http://192.0.2.1:12800", "http://192.0.2.1:12800", "http://192.0.2.1:12800/xiaozhi/ota/", "ws://192.0.2.1:12800/xiaozhi/ws"},
        {"https://example.test/", "https://example.test", "https://example.test/xiaozhi/ota/", "wss://example.test/xiaozhi/ws"},
        {"http://[2001:db8::1]:8080/", "http://[2001:db8::1]:8080", "http://[2001:db8::1]:8080/xiaozhi/ota/", "ws://[2001:db8::1]:8080/xiaozhi/ws"},
    };
    for (const auto& c : cases) {
        check(BuildServerEndpoints(c.input, ota, sizeof ota, ws, sizeof ws), c.input);
        check(std::strcmp(ota, c.ota) == 0 && std::strcmp(ws, c.ws) == 0, "generated endpoints");
        check(ServerBaseFromOta(ota, base, sizeof base) && std::strcmp(base, c.normalized) == 0, "OTA round trip");
        check(ServerBaseFromWebsocket(ws, base, sizeof base) && std::strcmp(base, c.normalized) == 0, "WS round trip");
    }
    const char* invalid_bases[] = {nullptr, "", "http://", "https://", "ftp://host", "http:///host", "http://host/path", "http://u@host", "http://host?q", "http://host#x", "http://host x", "http://host\\x"};
    for (const auto* s : invalid_bases) check(!BuildServerEndpoints(s, ota, sizeof ota, ws, sizeof ws), "invalid base");
    const char* invalid_ws[] = {nullptr, "", "http://host/xiaozhi/ws", "ws:///xiaozhi/ws", "ws://host/custom", "ws://host//xiaozhi/ws", "ws://host/path/xiaozhi/ws", "ws://u@host/xiaozhi/ws", "ws://host/xiaozhi/ws?x", "ws://host/xiaozhi/ws/"};
    for (const auto* s : invalid_ws) check(!ServerBaseFromWebsocket(s, base, sizeof base), "noncanonical WS");
    check(!ServerBaseFromOta("http://host/other", base, sizeof base), "wrong OTA suffix");
    check(!BuildServerEndpoints("http://host", ota, 4, ws, sizeof ws), "short OTA buffer");
    check(!BuildServerEndpoints("http://host", ota, sizeof ota, ws, 4), "short WS buffer");
    check(!BuildServerEndpoints("http://host", nullptr, sizeof ota, ws, sizeof ws), "null buffer");
    check(!ServerBaseFromWebsocket("ws://host/xiaozhi/ws", base, 4), "short reverse buffer");
    check(!ServerBaseFromOta("http://host/xiaozhi/ota/", base, 4), "short OTA reverse buffer");
    std::string max = "http://" + std::string(kMaxServerBase - 7, 'a');
    check(BuildServerEndpoints(max.c_str(), ota, sizeof ota, ws, sizeof ws), "max base");
    check(ServerBaseFromWebsocket(ws, base, sizeof base) && max == base, "max reverse");
    max += 'a';
    check(!BuildServerEndpoints(max.c_str(), ota, sizeof ota, ws, sizeof ws), "oversize base");
    std::puts("server_endpoint_test passed");
}
