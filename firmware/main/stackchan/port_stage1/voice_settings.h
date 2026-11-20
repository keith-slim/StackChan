#pragma once
// Stage-1 voice settings are allocated in PSRAM, not the ESP32 internal heap.
// Separate NVS namespace keeps the existing scheduler settings untouched.
#include <cstdint>
#include <cstddef>

namespace stackchan::stage1::voice {
struct Settings {
    char morning[513] = "おはよう。今日もよろしくね。";
    char bedtime[513] = "そろそろ寝るね。おやすみなさい。";
    char touch[513] = "びっくりした。起きたよ。";
    char wake_word[513] = "あ、呼んだ？ 聞いてるよ。";
    char timer[513] = "よく寝た。そろそろ起きるね。";
    char snore[257] = "グゥ～　グゥ～";
    char sleep_talk_prompt[257] = "寝言っぽい一言";
    char monologue_prompt[513] = "今の様子や気分について、短い独り言を一言。";
    uint16_t snore_min = 20, snore_max = 60;
    uint16_t talk_min = 60, talk_max = 180;
    uint16_t monologue_min = 5, monologue_max = 20;
    bool monologue_enabled = true;
};
bool Init();   // Lazily allocate persistent settings in SPIRAM.
void Stop();   // Not safe while async voice task is running.
const Settings* Current(); // Caller holds Stage-1 LVGL lock.
bool Save(const Settings& value); // Persist atomically before applying.
bool Valid(const Settings& value);
}
