#include "ip_store.h"
#include <esp_err.h>
#include <esp_log.h>
#include <esp_netif.h>

namespace {
constexpr char kTag[] = "StackChanWiFi";

void AssignIPv4(esp_ip4_addr_t& target, const uint8_t (&address)[4]) {
    target.addr = ESP_IP4TOADDR(address[0], address[1], address[2], address[3]);
}

esp_err_t ApplyDhcp(esp_netif_t* netif) {
    auto err = esp_netif_dhcpc_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return err;
    esp_netif_ip_info_t empty{};
    err = esp_netif_set_ip_info(netif, &empty);
    if (err != ESP_OK) return err;
    err = esp_netif_dhcpc_start(netif);
    return err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED ? ESP_OK : err;
}
} // namespace

// Called by the common WifiBoard callback immediately before the existing
// station connection attempt. Does not change SSID selection or credentials.
extern "C" esp_err_t stackchan_apply_saved_station_ip(const char* ssid) {
    if (!ssid || !*ssid) return ESP_ERR_INVALID_ARG;
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return ESP_ERR_INVALID_STATE;

    stackchan::wifi::IpSettings setting{};
    if (!stackchan::wifi::LoadIpSettings(ssid, setting)) {
        ESP_LOGW(kTag, "IPv4 settings unavailable; falling back to DHCP");
        return ApplyDhcp(netif);
    }
    if (!setting.use_static) {
        // Preserve the original DHCP flow for existing installations. Only
        // reset the address when switching away from a previous static SSID.
        esp_netif_dhcp_status_t status{};
        if (esp_netif_dhcpc_get_status(netif, &status) == ESP_OK &&
            status == ESP_NETIF_DHCP_STARTED) return ESP_OK;
        return ApplyDhcp(netif);
    }

    // ESP-IDF requires DHCP to be stopped before a static IPv4 address is set.
    auto err = esp_netif_dhcpc_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return err;
    esp_netif_ip_info_t info{};
    AssignIPv4(info.ip, setting.ip);
    AssignIPv4(info.netmask, setting.subnet);
    AssignIPv4(info.gw, setting.gateway);
    err = esp_netif_set_ip_info(netif, &info);
    if (err == ESP_OK) {
        const uint8_t (*addresses[])[4] = {&setting.dns1, &setting.dns2};
        for (unsigned index = 0; index < 2; ++index) {
            if (stackchan::wifi::IPv4Number(*addresses[index]) == 0) continue;
            esp_netif_dns_info_t dns{};
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            AssignIPv4(dns.ip.u_addr.ip4, *addresses[index]);
            err = esp_netif_set_dns_info(
                netif, index == 0 ? ESP_NETIF_DNS_MAIN : ESP_NETIF_DNS_BACKUP, &dns);
            if (err != ESP_OK) break;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Static IPv4 apply failed (%s); restoring DHCP",
                 esp_err_to_name(err));
        const auto fallback = ApplyDhcp(netif);
        return fallback == ESP_OK ? err : fallback;
    }
    ESP_LOGI(kTag, "Static IPv4 applied for saved SSID");
    return ESP_OK;
}
