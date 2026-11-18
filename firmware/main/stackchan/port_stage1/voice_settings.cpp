#include "voice_settings.h"
#include <nvs.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <cstring>
#include <new>

extern "C" void stackchan_nvs_commit_observer(void);

namespace stackchan::stage1::voice {
namespace {
Settings* current = nullptr;
constexpr const char* NS = "sc_voice";
struct TextField { const char* key; char* value; size_t size; };
}
bool Valid(const Settings& s) {
    if (s.snore_min < 1 || s.snore_min > s.snore_max || s.snore_max > 3600 ||
        s.talk_min < 1 || s.talk_min > s.talk_max || s.talk_max > 3600) return false;
    return std::memchr(s.morning, 0, sizeof(s.morning)) &&
           std::memchr(s.bedtime, 0, sizeof(s.bedtime)) &&
           std::memchr(s.touch, 0, sizeof(s.touch)) &&
           std::memchr(s.wake_word, 0, sizeof(s.wake_word)) &&
           std::memchr(s.timer, 0, sizeof(s.timer)) &&
           std::memchr(s.snore, 0, sizeof(s.snore)) &&
           std::memchr(s.sleep_talk_prompt, 0, sizeof(s.sleep_talk_prompt));
}
bool Init() {
    if (current) return true;
    auto* mem = heap_caps_malloc(sizeof(Settings), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) { ESP_LOGW("STAGE1-VOICE", "voice settings disabled: PSRAM unavailable"); return false; }
    current = new(mem) Settings();
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return true;
    TextField fields[] = {
        {"morning",current->morning,sizeof(current->morning)},
        {"bedtime",current->bedtime,sizeof(current->bedtime)},
        {"touch",current->touch,sizeof(current->touch)},
        {"wakeword",current->wake_word,sizeof(current->wake_word)},
        {"timer",current->timer,sizeof(current->timer)},
        {"snore",current->snore,sizeof(current->snore)},
        {"talkprompt",current->sleep_talk_prompt,sizeof(current->sleep_talk_prompt)},
    };
    for (const auto& item: fields) {
        size_t len=item.size;
        nvs_get_str(h,item.key,item.value,&len); // Missing keys keep S3 defaults.
    }
    nvs_get_u16(h,"snorelo",&current->snore_min);
    nvs_get_u16(h,"snorehi",&current->snore_max);
    nvs_get_u16(h,"talklo",&current->talk_min);
    nvs_get_u16(h,"talkhi",&current->talk_max);
    nvs_close(h);
    if (!Valid(*current)) {
        ESP_LOGW("STAGE1-VOICE","invalid NVS voice settings; restoring defaults in RAM");
        *current = Settings{};
    }
    return true;
}
void Stop() { if(current){current->~Settings();heap_caps_free(current);current=nullptr;} }
const Settings* Current() {return current;}
bool Save(const Settings& s) {
    if(!current||!Valid(s))return false;
    nvs_handle_t h;
    if(nvs_open(NS,NVS_READWRITE,&h)!=ESP_OK)return false;
    esp_err_t err=ESP_OK;
    const struct {const char* key;const char* text;} strings[]={
        {"morning",s.morning},{"bedtime",s.bedtime},
        {"touch",s.touch},{"wakeword",s.wake_word},{"timer",s.timer},
        {"snore",s.snore},{"talkprompt",s.sleep_talk_prompt},
    };
    for(const auto& field:strings)if(err==ESP_OK)err=nvs_set_str(h,field.key,field.text);
    if(err==ESP_OK)err=nvs_set_u16(h,"snorelo",s.snore_min);
    if(err==ESP_OK)err=nvs_set_u16(h,"snorehi",s.snore_max);
    if(err==ESP_OK)err=nvs_set_u16(h,"talklo",s.talk_min);
    if(err==ESP_OK)err=nvs_set_u16(h,"talkhi",s.talk_max);
    if(err==ESP_OK)err=nvs_commit(h);
    nvs_close(h);
    if(err==ESP_OK) {
        *current=s;
        stackchan_nvs_commit_observer();
    }
    else ESP_LOGE("STAGE1-VOICE","NVS save failed: %s",esp_err_to_name(err));
    return err==ESP_OK;
}
}
