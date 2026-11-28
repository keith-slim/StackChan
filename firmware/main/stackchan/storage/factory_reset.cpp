#include "factory_reset.h"
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_log.h>
#include <cstring>

namespace stackchan::storage {
namespace {
constexpr const char* kTag = "SC_FACTORY_RESET";
constexpr const char* kNamespace = "sc_reset";
constexpr const char* kKey = "pending";
esp_err_t WriteMarker(uint8_t pending) {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_set_u8(handle, kKey, pending);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle) nvs_close(handle);
    return err;
}
esp_err_t ReadMarker(uint8_t& pending) {
    pending = 0;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    err = nvs_get_u8(handle, kKey, &pending);
    nvs_close(handle);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}
} // namespace

esp_err_t RequestFactoryReset() {
    return WriteMarker(1);
}

esp_err_t CompletePendingFactoryReset() {
    uint8_t pending = 0;
    esp_err_t err = ReadMarker(pending);
    if (err != ESP_OK || !pending) return err;
    ESP_LOGI(kTag, "Completing requested factory reset before starting services");
    // Iterate one namespace at a time. Release the iterator BEFORE changing
    // NVS. The durable marker remains present across each committed erase,
    // so a reboot or power failure resumes the operation.
    for (unsigned pass = 0; pass < 256; ++pass) {
        nvs_iterator_t it = nullptr;
        err = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);
        char target[16]{};
        while (err == ESP_OK) {
            nvs_entry_info_t info{};
            err = nvs_entry_info(it, &info);
            if (err != ESP_OK) break;
            if (std::strcmp(info.namespace_name, kNamespace) != 0) {
                std::memcpy(target, info.namespace_name, sizeof(target));
                target[sizeof(target) - 1] = '\0';
                break;
            }
            err = nvs_entry_next(&it);
        }
        nvs_release_iterator(it);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
        if (!target[0]) {
            // Keep a committed completion marker: NVS is deliberately not
            // empty, so the existing empty-only SD recovery cannot import
            // pre-reset snapshots. Do not modify or erase SD files here.
            err = WriteMarker(0);
            if (err == ESP_OK) ESP_LOGI(kTag, "Factory reset complete; old SD recovery suppressed");
            return err;
        }
        nvs_handle_t handle = 0;
        err = nvs_open(target, NVS_READWRITE, &handle);
        if (err == ESP_OK) err = nvs_erase_all(handle);
        if (err == ESP_OK) err = nvs_commit(handle);
        if (handle) nvs_close(handle);
        if (err != ESP_OK) return err;
    }
    return ESP_ERR_INVALID_STATE;
}
} // namespace stackchan::storage
