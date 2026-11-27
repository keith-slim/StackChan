#include "nvs_snapshot.h"
#include "cores3_sd.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <memory>
#include <esp_heap_caps.h>
#include <string>
#include <utility>
#include <vector>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>
#include <nvs_flash.h>

namespace stackchan::sd {
namespace {
constexpr char kTag[] = "NVS-SD";
constexpr char kFormat[] = "stackchan-nvs-v1";
constexpr size_t kMaxItems = 512;
constexpr size_t kMaxValue = 32768;
struct Item {
    std::string ns, key, value;
    nvs_type_t type;
};
bool ValidName(const std::string& s) {
    if (s.empty() || s.size() > 15) return false;
    for (unsigned char c : s)
        if (c < 0x21 || c > 0x7e) return false;
    return true;
}
std::string Hex(const uint8_t* data, size_t len) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(len * 2, '0');
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 15];
    }
    return out;
}
int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool DecodeHex(const std::string& s, std::vector<uint8_t>& out) {
    if (s.size() % 2 || s.size() / 2 > kMaxValue) return false;
    out.resize(s.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        const int high = HexDigit(s[2 * i]), low = HexDigit(s[2 * i + 1]);
        if (high < 0 || low < 0) return false;
        out[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}
bool Unsigned(const std::string& s, uint64_t max, uint64_t& out) {
    if (s.empty() || s[0] < '0' || s[0] > '9') return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long val = std::strtoull(s.c_str(), &end, 10);
    if (errno != 0 || !end || *end || val > max) return false;
    out = val;
    return true;
}
bool Signed(const std::string& s, int64_t min, int64_t max, int64_t& out) {
    if (s.empty() || (s[0] != '-' && (s[0] < '0' || s[0] > '9'))) return false;
    char* end = nullptr;
    errno = 0;
    const long long val = std::strtoll(s.c_str(), &end, 10);
    if (errno != 0 || !end || *end || val < min || val > max) return false;
    out = val;
    return true;
}
bool ValidValue(const Item& item) {
    uint64_t u = 0; int64_t s = 0;
    switch (item.type) {
        case NVS_TYPE_U8: return Unsigned(item.value, UINT8_MAX, u);
        case NVS_TYPE_U16: return Unsigned(item.value, UINT16_MAX, u);
        case NVS_TYPE_U32: return Unsigned(item.value, UINT32_MAX, u);
        case NVS_TYPE_U64: return Unsigned(item.value, UINT64_MAX, u);
        case NVS_TYPE_I8: return Signed(item.value, INT8_MIN, INT8_MAX, s);
        case NVS_TYPE_I16: return Signed(item.value, INT16_MIN, INT16_MAX, s);
        case NVS_TYPE_I32: return Signed(item.value, INT32_MIN, INT32_MAX, s);
        case NVS_TYPE_I64: return Signed(item.value, INT64_MIN, INT64_MAX, s);
        case NVS_TYPE_STR: return item.value.size() <= kMaxValue;
        case NVS_TYPE_BLOB: {
            // Validate the hex text without allocating a redundant decoded BLOB.
            // DecodeHex() still converts it when NVS restoration actually needs it.
            if (item.value.size() % 2 != 0 || item.value.size() / 2 > kMaxValue) return false;
            for (char digit : item.value) {
                if (HexDigit(digit) < 0) return false;
            }
            return true;
        }
        default: return false;
    }
}
bool ReadValue(nvs_handle_t h, const nvs_entry_info_t& info, std::string& value) {
    const char* key = info.key;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    switch (info.type) {
        case NVS_TYPE_U8: { uint8_t v=0; err=nvs_get_u8(h,key,&v); if(err==ESP_OK)value=std::to_string(v); break; }
        case NVS_TYPE_I8: { int8_t v=0; err=nvs_get_i8(h,key,&v); if(err==ESP_OK)value=std::to_string(v); break; }
        case NVS_TYPE_U16: { uint16_t v=0; err=nvs_get_u16(h,key,&v); if(err==ESP_OK)value=std::to_string(v); break; }
        case NVS_TYPE_I16: { int16_t v=0; err=nvs_get_i16(h,key,&v); if(err==ESP_OK)value=std::to_string(v); break; }
        case NVS_TYPE_U32: { uint32_t v=0; err=nvs_get_u32(h,key,&v); if(err==ESP_OK)value=std::to_string(v); break; }
        case NVS_TYPE_I32: { int32_t v=0; err=nvs_get_i32(h,key,&v); if(err==ESP_OK)value=std::to_string(v); break; }
        case NVS_TYPE_U64: { uint64_t v=0; err=nvs_get_u64(h,key,&v); if(err==ESP_OK)value=std::to_string(v); break; }
        case NVS_TYPE_I64: { int64_t v=0; err=nvs_get_i64(h,key,&v); if(err==ESP_OK)value=std::to_string(v); break; }
        case NVS_TYPE_STR: {
            size_t n=0;
            err=nvs_get_str(h,key,nullptr,&n);
            if(err!=ESP_OK)break;
            if(n==0 || n>kMaxValue+1)return false;
            // Read-only staging buffer: explicitly external, no DMA use.
            std::unique_ptr<char, decltype(&heap_caps_free)> buf(
                static_cast<char*>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
                &heap_caps_free);
            if(!buf)return false;
            err=nvs_get_str(h,key,buf.get(),&n);
            if(err==ESP_OK)value.assign(buf.get());
            break;
        }
        case NVS_TYPE_BLOB: {
            size_t n=0;
            err=nvs_get_blob(h,key,nullptr,&n);
            if(err!=ESP_OK)break;
            if(n>kMaxValue)return false;
            std::unique_ptr<uint8_t, decltype(&heap_caps_free)> buf(
                n ? static_cast<uint8_t*>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) : nullptr,
                &heap_caps_free);
            if(n && !buf)return false;
            const size_t capacity = n;
            err=nvs_get_blob(h,key,buf.get(),&n);
            if(err==ESP_OK && n<=capacity)value=Hex(buf.get(),n);
            else return false;
            break;
        }
        default: return false;
    }
    return err==ESP_OK;
}
esp_err_t WriteValue(nvs_handle_t h, const Item& item) {
    const char* key=item.key.c_str();
    uint64_t u=0; int64_t s=0;
    if (!ValidValue(item)) return ESP_ERR_INVALID_ARG;
    switch(item.type) {
        case NVS_TYPE_U8: Unsigned(item.value,UINT8_MAX,u); return nvs_set_u8(h,key,static_cast<uint8_t>(u));
        case NVS_TYPE_I8: Signed(item.value,INT8_MIN,INT8_MAX,s); return nvs_set_i8(h,key,static_cast<int8_t>(s));
        case NVS_TYPE_U16: Unsigned(item.value,UINT16_MAX,u); return nvs_set_u16(h,key,static_cast<uint16_t>(u));
        case NVS_TYPE_I16: Signed(item.value,INT16_MIN,INT16_MAX,s); return nvs_set_i16(h,key,static_cast<int16_t>(s));
        case NVS_TYPE_U32: Unsigned(item.value,UINT32_MAX,u); return nvs_set_u32(h,key,static_cast<uint32_t>(u));
        case NVS_TYPE_I32: Signed(item.value,INT32_MIN,INT32_MAX,s); return nvs_set_i32(h,key,static_cast<int32_t>(s));
        case NVS_TYPE_U64: Unsigned(item.value,UINT64_MAX,u); return nvs_set_u64(h,key,u);
        case NVS_TYPE_I64: Signed(item.value,INT64_MIN,INT64_MAX,s); return nvs_set_i64(h,key,s);
        case NVS_TYPE_STR: return nvs_set_str(h,key,item.value.c_str());
        case NVS_TYPE_BLOB: {
            std::vector<uint8_t> buf;
            if(!DecodeHex(item.value,buf))return ESP_ERR_INVALID_ARG;
            return nvs_set_blob(h,key,buf.empty() ? static_cast<const void*>("") :
                                  static_cast<const void*>(buf.data()),buf.size());
        }
        default: return ESP_ERR_INVALID_ARG;
    }
}
// Deterministic FNV64 over the serialized entries. It detects accidental FAT
// corruption without adding encryption or assuming NVS data is public.
std::string Checksum(const char* data) {
    uint64_t hash=UINT64_C(14695981039346656037);
    for(const unsigned char* p=reinterpret_cast<const unsigned char*>(data);*p;++p) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    char text[17];
    std::snprintf(text,sizeof(text),"%016llx",static_cast<unsigned long long>(hash));
    return text;
}
bool ParseSnapshot(const std::string& text,std::vector<Item>& items);
bool BuildSnapshot(std::string& out) {
    std::vector<Item> items;
    nvs_iterator_t it=nullptr;
    esp_err_t err=nvs_entry_find(NVS_DEFAULT_PART_NAME,nullptr,NVS_TYPE_ANY,&it);
    while(err==ESP_OK) {
        nvs_entry_info_t info={};
        if(nvs_entry_info(it,&info)!=ESP_OK){err=ESP_FAIL;break;}
        nvs_handle_t handle=0;
        if(nvs_open(info.namespace_name,NVS_READONLY,&handle)!=ESP_OK){err=ESP_FAIL;break;}
        Item item{info.namespace_name,info.key,{},info.type};
        const bool ok=ReadValue(handle,info,item.value);
        nvs_close(handle);
        if(!ok || !ValidName(item.ns) || !ValidName(item.key) || !ValidValue(item)) {err=ESP_FAIL;break;}
        items.push_back(std::move(item));
        if(items.size()>kMaxItems){err=ESP_ERR_INVALID_SIZE;break;}
        err=nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    if(err!=ESP_ERR_NVS_NOT_FOUND || items.empty())return false;
    std::sort(items.begin(),items.end(),[](const Item& a,const Item& b){
        return a.ns == b.ns ? a.key < b.key : a.ns < b.ns;
    });
    cJSON* root=cJSON_CreateObject();
    cJSON* entries=cJSON_CreateArray();
    if(!root || !entries){cJSON_Delete(root);cJSON_Delete(entries);return false;}
    cJSON_AddStringToObject(root,"format",kFormat);
    cJSON_AddNumberToObject(root,"version",1);
    cJSON_AddItemToObject(root,"entries",entries);
    for(const auto& item:items) {
        cJSON* row=cJSON_CreateObject();
        if(!row){cJSON_Delete(root);return false;}
        cJSON_AddStringToObject(row,"namespace",item.ns.c_str());
        cJSON_AddStringToObject(row,"key",item.key.c_str());
        cJSON_AddNumberToObject(row,"type",static_cast<int>(item.type));
        cJSON_AddStringToObject(row,"value",item.value.c_str());
        cJSON_AddItemToArray(entries,row);
    }
    // All source values are now owned by cJSON; free their temporary copies
    // before building the two serialized representations and validation copy.
    const size_t expected_items = items.size();
    std::vector<Item>().swap(items);
    char* payload=cJSON_PrintUnformatted(entries);
    if(!payload){cJSON_Delete(root);return false;}
    const std::string checksum=Checksum(payload);
    cJSON_free(payload);
    cJSON_AddStringToObject(root,"checksum",checksum.c_str());
    char* serialized=cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if(!serialized)return false;
    out.assign(serialized);
    cJSON_free(serialized);
    // Do not publish an incomplete snapshot if a cJSON allocation failed.
    std::vector<Item> confirmed;
    return ParseSnapshot(out,confirmed) && confirmed.size()==expected_items;
}
bool ParseSnapshot(const std::string& text,std::vector<Item>& items) {
    cJSON* root=cJSON_ParseWithLength(text.data(),text.size());
    if(!root)return false;
    const auto* format=cJSON_GetObjectItemCaseSensitive(root,"format");
    const auto* version=cJSON_GetObjectItemCaseSensitive(root,"version");
    const auto* checksum=cJSON_GetObjectItemCaseSensitive(root,"checksum");
    const auto* entries=cJSON_GetObjectItemCaseSensitive(root,"entries");
    bool ok=cJSON_IsString(format) && std::strcmp(format->valuestring,kFormat)==0 &&
        cJSON_IsNumber(version) && version->valueint==1 && cJSON_IsString(checksum) &&
        cJSON_IsArray(entries) && cJSON_GetArraySize(entries)>0 &&
        static_cast<size_t>(cJSON_GetArraySize(entries))<=kMaxItems;
    if(ok) {
        char* serialized=cJSON_PrintUnformatted(entries);
        ok=serialized && Checksum(serialized)==checksum->valuestring;
        cJSON_free(serialized);
    }
    if(ok) {
        cJSON* row=nullptr;
        cJSON_ArrayForEach(row,entries) {
            const auto* ns=cJSON_GetObjectItemCaseSensitive(row,"namespace");
            const auto* key=cJSON_GetObjectItemCaseSensitive(row,"key");
            const auto* type=cJSON_GetObjectItemCaseSensitive(row,"type");
            const auto* value=cJSON_GetObjectItemCaseSensitive(row,"value");
            if(!cJSON_IsString(ns)||!cJSON_IsString(key)||
               !cJSON_IsNumber(type)||!cJSON_IsString(value)) {ok=false;break;}
            Item item{ns->valuestring,key->valuestring,value->valuestring,
                      static_cast<nvs_type_t>(type->valueint)};
            if(!ValidName(item.ns)||!ValidName(item.key)||!ValidValue(item)) {ok=false;break;}
            items.push_back(std::move(item));
        }
    }
    if(ok) {
        // Compare existing entries by index. Unlike a set of (namespace,key)
        // strings, this does not allocate another copy of every key, and
        // it leaves the original NVS restore order untouched.
        std::vector<size_t> order(items.size());
        std::iota(order.begin(),order.end(),0);
        std::sort(order.begin(),order.end(),[&](size_t a,size_t b) {
            return items[a].ns == items[b].ns
                ? items[a].key < items[b].key : items[a].ns < items[b].ns;
        });
        for(size_t i=1;i<order.size();++i) {
            const Item& previous=items[order[i-1]];
            const Item& current=items[order[i]];
            if(previous.ns == current.ns && previous.key == current.key) {
                ok=false;
                break;
            }
        }
    }
    cJSON_Delete(root);
    if(!ok)items.clear();
    return ok;
}
bool IsNvsEmpty(bool& empty) {
    nvs_iterator_t it=nullptr;
    const esp_err_t err=nvs_entry_find(NVS_DEFAULT_PART_NAME,nullptr,NVS_TYPE_ANY,&it);
    nvs_release_iterator(it);
    if(err==ESP_ERR_NVS_NOT_FOUND){empty=true;return true;}
    if(err==ESP_OK){empty=false;return true;}
    return false;
}
} // namespace

bool RestoreNvsIfEmpty() {
    bool empty=false;
    if(!IsNvsEmpty(empty)||!empty)return false;
    std::string text;
    if(!ReadConfig(text))return false;
    std::vector<Item> items;
    if(!ParseSnapshot(text,items)) {
        ESP_LOGW(kTag,"SD config is not a valid StackChan NVS snapshot; leaving NVS unchanged");
        return false;
    }
    // Validate every value before writing ANY NVS entry. Commit per namespace.
    std::vector<std::pair<std::string,nvs_handle_t>> handles;
    esp_err_t err=ESP_OK;
    for(const auto& item:items) {
        auto found=std::find_if(handles.begin(),handles.end(),[&](const auto& h){return h.first==item.ns;});
        if(found==handles.end()) {
            nvs_handle_t h=0;
            err=nvs_open(item.ns.c_str(),NVS_READWRITE,&h);
            if(err!=ESP_OK)break;
            handles.emplace_back(item.ns,h);
            found=handles.end()-1;
        }
        err=WriteValue(found->second,item);
        if(err!=ESP_OK)break;
    }
    if(err==ESP_OK) for(const auto& entry:handles) {
        err=nvs_commit(entry.second);
        if(err!=ESP_OK)break;
    }
    if(err!=ESP_OK) {
        // NVS had no keys before this operation. Roll back the keys written by
        // this attempt and never allow a partial recovery to be called success.
        ESP_LOGE(kTag,"NVS restoration failed: %s",esp_err_to_name(err));
        for(const auto& entry:handles) {
            nvs_erase_all(entry.second);
            nvs_commit(entry.second);
        }
    }
    for(const auto& entry:handles)nvs_close(entry.second);
    if(err==ESP_OK)ESP_LOGI(kTag,"Restored %u NVS entries from SD; reboot required",
                           static_cast<unsigned>(items.size()));
    return err==ESP_OK;
}

bool BackupNvsIfChanged() {
    static std::string last_confirmed;
    static int64_t last_failure_us=0;
    std::string text;
    if(!BuildSnapshot(text))return false;
    if(text==last_confirmed)return true; // Avoid repeated SD mounts when nothing changed.
    const int64_t now_us=esp_timer_get_time();
    if(last_failure_us && now_us-last_failure_us<60000000LL)return false; // Missing SD: retry at most once a minute.
    std::string previous;
    if(ReadConfig(previous)) {
        if(previous==text){last_confirmed=text;return true;}
        std::vector<Item> items;
        if(!ParseSnapshot(previous,items)) {
            ESP_LOGW(kTag,"Existing SD config belongs to another format; will not overwrite it");
            last_failure_us=now_us;
            return false;
        }
    }
    const bool ok=WriteConfig(text);
    if(ok) {
        last_confirmed=text;
        last_failure_us=0;
        ESP_LOGI(kTag,"NVS snapshot saved to SD");
    } else {
        last_failure_us=now_us;
    }
    return ok;
}
} // namespace stackchan::sd
