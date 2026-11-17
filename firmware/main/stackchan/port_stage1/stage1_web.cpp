// Local Stage-1 settings page; independent of the Xiaozhi WebSocket.
#include "stage1_web.h"
#include "port_stage1.h"
#include <hal/hal.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <cJSON.h>
#include <cstdio>
#include <cstring>

namespace stackchan::stage1::web {
namespace {
constexpr const char* TAG = "STAGE1-WEB";
httpd_handle_t server = nullptr;

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
<fieldset><legend>自動演出</legend>
<label><input name="theatre" type="checkbox"> 小芝居を有効にする（1～3分間隔）</label>
<label><input name="naps" type="checkbox"> 昼寝を有効にする</label>
<label>昼寝の確率（％） <input name="chance" type="number" min="0" max="100" step="5" required></label>
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
  status.textContent='読み込み完了';
 }catch(e){status.textContent='読み込み失敗: '+e.message;}
}
form.addEventListener('submit',async e=>{
 e.preventDefault();
 if(!form.reportValidity())return;
 if(f.wake.value===f.sleep.value){status.textContent='起床・就寝時刻は異なる値にしてください';return;}
 const data={wake_minute:minute(f.wake.value),sleep_minute:minute(f.sleep.value),
 theatre:f.theatre.checked,naps:f.naps.checked,nap_chance:Number(f.chance.value)};
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
    char json[180];
    std::snprintf(json, sizeof(json),
        "{\"wake_minute\":%u,\"sleep_minute\":%u,\"theatre\":%s,\"naps\":%s,\"nap_chance\":%u}",
        static_cast<unsigned>(c.wake_minute), static_cast<unsigned>(c.sleep_minute),
        c.theatre ? "true" : "false", c.naps ? "true" : "false",
        static_cast<unsigned>(c.nap_chance));
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
    if (req->content_len <= 0 || req->content_len > 256) {
        Reply(req, "413 Payload Too Large", "{\"error\":\"invalid size\"}");
        return ESP_OK;
    }
    char buf[257]{};
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, buf + received, req->content_len - received);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return ESP_FAIL;
        received += n;
    }
    cJSON* root = cJSON_Parse(buf);
    int wake = 0, sleep = 0, chance = 0;
    const cJSON* theatre = root ? cJSON_GetObjectItemCaseSensitive(root, "theatre") : nullptr;
    const cJSON* naps = root ? cJSON_GetObjectItemCaseSensitive(root, "naps") : nullptr;
    const bool valid = root && cJSON_IsObject(root) &&
        Uint(root, "wake_minute", 0, 1439, 1, wake) &&
        Uint(root, "sleep_minute", 0, 1439, 1, sleep) &&
        Uint(root, "nap_chance", 0, 100, 5, chance) &&
        cJSON_IsBool(theatre) && cJSON_IsBool(naps) && wake != sleep;
    if (valid) {
        Config c{};
        c.wake_minute = wake;
        c.sleep_minute = sleep;
        c.theatre = cJSON_IsTrue(theatre);
        c.naps = cJSON_IsTrue(naps);
        c.nap_chance = chance;
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
    if (server) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 4096;
    config.max_uri_handlers = 3;
    const esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "port 80 start failed: %s", esp_err_to_name(err));
        server = nullptr;
        return;
    }
    const httpd_uri_t page{.uri="/", .method=HTTP_GET, .handler=Page, .user_ctx=nullptr};
    const httpd_uri_t read{.uri="/api/stage1", .method=HTTP_GET, .handler=ReadConfig, .user_ctx=nullptr};
    const httpd_uri_t write{.uri="/api/stage1", .method=HTTP_POST, .handler=WriteConfig, .user_ctx=nullptr};
    if (httpd_register_uri_handler(server, &page) != ESP_OK ||
        httpd_register_uri_handler(server, &read) != ESP_OK ||
        httpd_register_uri_handler(server, &write) != ESP_OK) {
        ESP_LOGE(TAG, "URI registration failed");
        httpd_stop(server);
        server = nullptr;
        return;
    }
    ESP_LOGI(TAG, "settings available on http://<device-ip>/");
}
}
