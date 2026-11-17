// Local Stage-1 settings page; independent of the Xiaozhi WebSocket.
#include "stage1_web.h"
#include "port_stage1.h"
#include <hal/hal.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <cJSON.h>
#include <cstdio>
#include <cstring>

namespace stackchan::stage1::web {
namespace {
constexpr const char* TAG = "STAGE1-WEB";
httpd_handle_t server = nullptr;
std::atomic<bool> attempted{false};
constexpr size_t MIN_FREE_INTERNAL = 18000;
constexpr size_t MIN_LARGEST_INTERNAL = 5500;

constexpr char PAGE[] = R"STAGE1PAGE(<!doctype html>
<html lang="ja"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>StackChan 工程1設定</title><style>
:root{font-family:system-ui,sans-serif;color-scheme:light dark}
body{max-width:550px;padding:20px;margin:auto;line-height:1.6}
h1{font-size:1.4rem}fieldset{border:1px solid #888;border-radius:9px;margin:12px 0;padding:14px}
label{display:block;margin:10px 0}input[type=time],input[type=number]{font-size:1.1rem;padding:6px}
button{font-size:1rem;padding:10px 24px}#status{min-height:2em;white-space:pre-wrap}
</style></head><body><h1>StackChan 工程1設定</h1>
<p>保存すると直ちに反映され、再起動後も保持されます。</p>
<form id="settings">
<fieldset><legend>起床・就寝</legend>
<label>起床時刻 <input name="wake" type="time" required></label>
<label>就寝時刻 <input name="sleep" type="time" required></label>
</fieldset>
<fieldset><legend>待機中の小芝居</legend>
<label><input name="theatre" type="checkbox"> 小芝居を有効にする</label>
<label>無操作時間・最短（秒） <input name="theatreMin" type="number" min="10" max="3600" step="10" required></label>
<label>無操作時間・最長（秒） <input name="theatreMax" type="number" min="10" max="3600" step="10" required></label>
<p>くしゃみ・しゃっくり・空腹・背伸び・ひらめき・照れ笑い・あくび。音声は使用しません。</p>
</fieldset>
<fieldset><legend>昼寝</legend>
<label><input name="naps" type="checkbox"> 昼寝を有効にする</label>
<label>昼寝する確率（％） <input name="chance" type="number" min="0" max="100" step="5" required></label>
<label>昼寝抽選まで・最短（分） <input name="napIdleMin" type="number" min="1" max="1440" required></label>
<label>昼寝抽選まで・最長（分） <input name="napIdleMax" type="number" min="1" max="1440" required></label>
<label>昼寝の継続時間・最短（分） <input name="napSleepMin" type="number" min="1" max="1440" required></label>
<label>昼寝の継続時間・最長（分） <input name="napSleepMax" type="number" min="1" max="1440" required></label>
<p>昼寝に当選すると約30秒の眠気・あくび演出の後に眠ります。夢の絵を表示し、設定時間で目覚めます。</p>
</fieldset><button type="submit">保存して反映</button></form>
<p id="status" role="status">読み込み中…</p>
<script>
const form=document.getElementById('settings'),status=document.getElementById('status');
const f=form.elements;
const time=n=>String(Math.floor(n/60)).padStart(2,'0')+':'+String(n%60).padStart(2,'0');
const minute=t=>{const [h,m]=t.split(':').map(Number);return h*60+m;};
async function load(){
 try {
  const res=await fetch('/api/stage1',{cache:'no-store'});
  if(!res.ok)throw Error('HTTP '+res.status);
  const c=await res.json();
  f.wake.value=time(c.wake_minute);f.sleep.value=time(c.sleep_minute);
  f.theatre.checked=c.theatre;f.naps.checked=c.naps;f.chance.value=c.nap_chance;
  f.theatreMin.value=c.theatre_min_seconds;f.theatreMax.value=c.theatre_max_seconds;
  f.napIdleMin.value=c.nap_idle_min_minutes;f.napIdleMax.value=c.nap_idle_max_minutes;
  f.napSleepMin.value=c.nap_sleep_min_minutes;f.napSleepMax.value=c.nap_sleep_max_minutes;
  status.textContent='読み込み完了';
 }catch(e){status.textContent='読み込み失敗: '+e.message;}
}
form.addEventListener('submit',async e=>{
 e.preventDefault();
 if(!form.reportValidity())return;
 if(f.wake.value===f.sleep.value){status.textContent='起床・就寝時刻は異なる値にしてください';return;}
 for(const [lo,hi] of [['theatreMin','theatreMax'],['napIdleMin','napIdleMax'],['napSleepMin','napSleepMax']]){
  if(Number(f[lo].value)>Number(f[hi].value)){
   status.textContent='最短時間は最長時間以下にしてください';return;
  }
 }
 const data={wake_minute:minute(f.wake.value),sleep_minute:minute(f.sleep.value),
 theatre:f.theatre.checked,naps:f.naps.checked,nap_chance:Number(f.chance.value),
 theatre_min_seconds:Number(f.theatreMin.value),theatre_max_seconds:Number(f.theatreMax.value),
 nap_idle_min_minutes:Number(f.napIdleMin.value),nap_idle_max_minutes:Number(f.napIdleMax.value),
 nap_sleep_min_minutes:Number(f.napSleepMin.value),nap_sleep_max_minutes:Number(f.napSleepMax.value)};
 status.textContent='保存中…';
 try{
  const res=await fetch('/api/stage1',{method:'POST',
   headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});
  if(!res.ok)throw Error('HTTP '+res.status+': '+await res.text());
  status.textContent='保存しました。設定は反映済みです。';
 }catch(e){status.textContent='保存失敗: '+e.message;}
});
load();
</script></body></html>)STAGE1PAGE";

void Reply(httpd_req_t* req, const char* status, const char* body) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, body);
}
esp_err_t Page(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}
esp_err_t ReadConfig(httpd_req_t* req) {
    Config c;
    {
        LvglLockGuard lock;
        c = GetConfig();
    }
    char json[384];
    std::snprintf(json, sizeof(json),
        "{\"wake_minute\":%u,\"sleep_minute\":%u,\"theatre\":%s,\"naps\":%s,\"nap_chance\":%u,"
        "\"theatre_min_seconds\":%u,\"theatre_max_seconds\":%u,"
        "\"nap_idle_min_minutes\":%u,\"nap_idle_max_minutes\":%u,"
        "\"nap_sleep_min_minutes\":%u,\"nap_sleep_max_minutes\":%u}",
        static_cast<unsigned>(c.wake_minute), static_cast<unsigned>(c.sleep_minute),
        c.theatre ? "true" : "false", c.naps ? "true" : "false",
        static_cast<unsigned>(c.nap_chance),
        static_cast<unsigned>(c.theatre_min_seconds), static_cast<unsigned>(c.theatre_max_seconds),
        static_cast<unsigned>(c.nap_idle_min_minutes), static_cast<unsigned>(c.nap_idle_max_minutes),
        static_cast<unsigned>(c.nap_sleep_min_minutes), static_cast<unsigned>(c.nap_sleep_max_minutes));
    Reply(req, "200 OK", json);
    return ESP_OK;
}
bool Uint(const cJSON* root, const char* key, int min, int max, int step, int& value) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item)) return false;
    const double n = item->valuedouble;
    if (n < min || n > max || n != static_cast<int>(n) || static_cast<int>(n) % step) return false;
    value = static_cast<int>(n);
    return true;
}
esp_err_t WriteConfig(httpd_req_t* req) {
    char content_type[64]{};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        std::strncmp(content_type, "application/json", 16) != 0) {
        Reply(req, "415 Unsupported Media Type", "{\"error\":\"JSON required\"}");
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > 512) {
        Reply(req, "413 Payload Too Large", "{\"error\":\"invalid size\"}");
        return ESP_OK;
    }
    char buf[513]{};
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, buf + received, req->content_len - received);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return ESP_FAIL;
        received += n;
    }
    cJSON* root = cJSON_Parse(buf);
    int wake = 0, sleep = 0, chance = 0;
    int theatre_min = 0, theatre_max = 0, nap_idle_min = 0, nap_idle_max = 0;
    int nap_sleep_min = 0, nap_sleep_max = 0;
    const cJSON* theatre = root ? cJSON_GetObjectItemCaseSensitive(root, "theatre") : nullptr;
    const cJSON* naps = root ? cJSON_GetObjectItemCaseSensitive(root, "naps") : nullptr;
    const bool valid = root && cJSON_IsObject(root) &&
        Uint(root, "wake_minute", 0, 1439, 1, wake) &&
        Uint(root, "sleep_minute", 0, 1439, 1, sleep) &&
        Uint(root, "nap_chance", 0, 100, 5, chance) &&
        Uint(root, "theatre_min_seconds", 10, 3600, 10, theatre_min) &&
        Uint(root, "theatre_max_seconds", 10, 3600, 10, theatre_max) &&
        Uint(root, "nap_idle_min_minutes", 1, 1440, 1, nap_idle_min) &&
        Uint(root, "nap_idle_max_minutes", 1, 1440, 1, nap_idle_max) &&
        Uint(root, "nap_sleep_min_minutes", 1, 1440, 1, nap_sleep_min) &&
        Uint(root, "nap_sleep_max_minutes", 1, 1440, 1, nap_sleep_max) &&
        cJSON_IsBool(theatre) && cJSON_IsBool(naps) && wake != sleep &&
        theatre_min <= theatre_max && nap_idle_min <= nap_idle_max &&
        nap_sleep_min <= nap_sleep_max;
    if (valid) {
        Config c{};
        c.wake_minute = wake;
        c.sleep_minute = sleep;
        c.theatre = cJSON_IsTrue(theatre);
        c.naps = cJSON_IsTrue(naps);
        c.nap_chance = chance;
        c.theatre_min_seconds = theatre_min;
        c.theatre_max_seconds = theatre_max;
        c.nap_idle_min_minutes = nap_idle_min;
        c.nap_idle_max_minutes = nap_idle_max;
        c.nap_sleep_min_minutes = nap_sleep_min;
        c.nap_sleep_max_minutes = nap_sleep_max;
        bool saved;
        {
            LvglLockGuard lock;
            saved = SetConfig(c);
        }
        Reply(req, saved ? "200 OK" : "500 Internal Server Error",
              saved ? "{\"ok\":true}" : "{\"error\":\"NVS save failed\"}");
    } else Reply(req, "400 Bad Request", "{\"error\":\"invalid configuration\"}");
    cJSON_Delete(root);
    return ESP_OK;
}
}

void Start() {
    // Never start during boot/audio initialization. Called once from the
    // already-running StackChan update task, only after Xiaozhi is idle.
    if (attempted.exchange(true)) return;
    const size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "deferred start: internal_free=%u largest=%u",
             static_cast<unsigned>(free_bytes), static_cast<unsigned>(largest));
    if (free_bytes < MIN_FREE_INTERNAL || largest < MIN_LARGEST_INTERNAL) {
        ESP_LOGW(TAG, "web disabled: insufficient internal heap; avatar/audio unaffected");
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 4096;
    config.max_uri_handlers = 3;
    config.max_open_sockets = 4;
    const esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "port 80 start failed: %s", esp_err_to_name(err));
        server = nullptr;
        return;
    }
    httpd_uri_t page{};
    page.uri = "/"; page.method = HTTP_GET; page.handler = Page;
    httpd_uri_t read{};
    read.uri = "/api/stage1"; read.method = HTTP_GET; read.handler = ReadConfig;
    httpd_uri_t write{};
    write.uri = "/api/stage1"; write.method = HTTP_POST; write.handler = WriteConfig;
    if (httpd_register_uri_handler(server, &page) != ESP_OK ||
        httpd_register_uri_handler(server, &read) != ESP_OK ||
        httpd_register_uri_handler(server, &write) != ESP_OK) {
        ESP_LOGE(TAG, "URI registration failed");
        httpd_stop(server);
        server = nullptr;
        return;
    }
    ESP_LOGI(TAG, "ready: port=80 internal_free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}
}
