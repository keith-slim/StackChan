// Local Stage-1 settings page; independent of the Xiaozhi WebSocket.
#include "stage1_web.h"
#include "port_stage1.h"
#include "voice_settings.h"
#include "voice_client.h"
#include "stackchan/wifi/wifi_web.h"
#include <hal/hal.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <cJSON.h>
#include <cstdio>
#include <cstring>
#include <new>

namespace stackchan::stage1::web {
namespace {
constexpr const char* TAG = "STAGE1-WEB";
httpd_handle_t server = nullptr;
std::atomic<bool> attempted{false};
constexpr size_t MIN_FREE_INTERNAL = 18000;
constexpr size_t MIN_LARGEST_INTERNAL = 5500;

constexpr char PAGE[] = R"STAGE1PAGE(<!doctype html><html lang="ja"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>StackChan 設定</title><style>
:root{font:13px system-ui,sans-serif;color:#252f42;background:#eef1f5;line-height:1.6}
*{box-sizing:border-box}body{margin:0}
.app{display:grid;grid-template-columns:172px minmax(0,1fr);max-width:1080px;min-height:100vh;margin:auto}
.sidebar{position:sticky;top:0;height:100vh;background:#182a46;padding:24px 12px;color:#fff}
.brand{display:block;font-size:18px;font-weight:700;color:white;padding:0 12px 20px}
nav{display:grid;gap:6px}nav a{display:block;color:#dbe6f6;text-decoration:none;padding:11px 14px;border-radius:8px}
nav a:hover,nav a:focus{background:#285bd4}
main{padding:18px;min-width:0}h1{font-size:19px;margin:0 0 12px}h2{font-size:16px;margin:0 0 12px}
p{margin:8px 0}small,.hint{display:block;color:#68758a;font-size:10.5px;line-height:1.7}
.card{background:#fff;border:1px solid #d6dce7;border-radius:8px;padding:14px;margin-bottom:12px}
.fields{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}
label{display:block;margin:0 0 5px;font-weight:500}
input,button{font:inherit}
input[type=time],input[type=number],input[type=text],textarea{display:block;width:100%;max-width:14rem;min-height:30px;border:1px solid #bac5d6;border-radius:7px;background:#fff;color:inherit;padding:5px 8px}
textarea{display:block;resize:vertical;min-height:60px;width:100%;max-width:none}
.check{display:flex;gap:8px;align-items:center;margin:0 0 12px}
input[type=checkbox]{width:16px;height:16px;margin:0}
button{min-height:30px;padding:5px 12px;background:#285bd4;color:white;border:0;border-radius:7px;cursor:pointer}
button:hover{background:#204bb4}button:disabled{opacity:.45;cursor:default}
.buttons{display:flex;flex-wrap:wrap;gap:8px}
.status{min-height:1.6em;white-space:pre-wrap;overflow-wrap:anywhere;color:#465976;margin:8px 0 0}
.save{margin:0 0 16px}.save button{font-weight:600}
@media(max-width:740px){
 .app{display:block}.sidebar{position:static;height:auto;padding:10px 12px}
 .brand{display:inline-block;font-size:16px;padding:3px 10px 8px}
 nav{display:flex;flex-wrap:wrap;gap:4px}nav a{padding:6px 10px}
 main{padding:12px}
}
@media(max-width:440px){.fields{grid-template-columns:1fr}}
</style></head><body>
<div class="app"><aside class="sidebar"><strong class="brand">StackChan</strong>
<nav aria-label="設定メニュー">
<a href="#daily">起床・就寝</a><a href="#theatre">小芝居</a><a href="#nap">昼寝</a><a href="#voice">音声・文言</a><a href="#actions">サーボテスト</a>
</nav></aside><main>
<h1>セッティング</h1>
<form id="settings">
<section id="daily" class="card"><h2>起床・就寝</h2>
<div class="fields">
<label>起床時刻<input name="wake" type="time" required></label>
<label>就寝時刻<input name="sleep" type="time" required></label>
</div></section>
<section id="theatre" class="card"><h2>待機中の小芝居</h2>
<label class="check"><input name="theatre" type="checkbox"> 小芝居を有効にする</label>
<div class="fields">
<label>無操作時間・最短（秒）<input name="theatreMin" type="number" min="10" max="3600" step="10" required></label>
<label>無操作時間・最長（秒）<input name="theatreMax" type="number" min="10" max="3600" step="10" required></label>
</div><small>7種類の短い無音演出。会話中は行いません。</small></section>
<section id="nap" class="card"><h2>昼寝</h2>
<label class="check"><input name="naps" type="checkbox"> 昼寝を有効にする</label>
<div class="fields">
<label>昼寝する確率（％）<input name="chance" type="number" min="0" max="100" step="5" required></label>
<div></div>
<label>抽選まで・最短（分）<input name="napIdleMin" type="number" min="1" max="1440" required></label>
<label>抽選まで・最長（分）<input name="napIdleMax" type="number" min="1" max="1440" required></label>
<label>継続時間・最短（分）<input name="napSleepMin" type="number" min="1" max="1440" required></label>
<label>継続時間・最長（分）<input name="napSleepMax" type="number" min="1" max="1440" required></label>
</div><small>昼寝当選後、眠気・あくびを経て就寝。夢の絵を表示します。</small></section>
<div class="save"><button type="submit">保存して反映</button>
<p id="status" class="status" role="status">読み込み中…</p></div>
</form>
<form id="voiceSettings">
<section id="voice" class="card"><h2>Gemini Live発話・文言（既存WebSocket 12800）</h2>
<small>音声は既存の会話サーバーと接続設定を共有します。別の発話用サーバー・ポート設定は不要です。</small></section>
<section class="card"><h2>朝・夜の文言</h2><div class="fields">
<label>朝の定時起床<textarea name="morning" rows="2"></textarea></label>
<label>夜の就寝案内<textarea name="bedtime" rows="2"></textarea></label>
</div><small>各512バイトまで。空欄は発話しません。</small></section>
<section class="card"><h2>昼寝から起きたとき</h2><div class="fields">
<label>パッド・スクリーンタッチ<textarea name="touch" rows="2"></textarea></label>
<label>ウェイクワード<textarea name="wake_word" rows="2"></textarea></label>
<label>タイマー<textarea name="timer" rows="2"></textarea></label>
</div><small>起床理由ごとの文言（各512バイト）。空欄は省略します。</small></section>
<section class="card"><h2>昼寝中のいびき・寝言</h2>
<label>いびきの文言<textarea name="snore" rows="2"></textarea></label>
<div class="fields">
<label>いびき最短間隔（秒）<input name="snore_min" type="number" min="1" max="3600" required></label>
<label>いびき最長間隔（秒）<input name="snore_max" type="number" min="1" max="3600" required></label>
</div>
<label>寝言用のGemini Liveへの指示<textarea name="sleep_talk_prompt" rows="3"></textarea></label>
<div class="fields">
<label>寝言最短間隔（秒）<input name="talk_min" type="number" min="1" max="3600" required></label>
<label>寝言最長間隔（秒）<input name="talk_max" type="number" min="1" max="3600" required></label>
</div><small>各256バイトまで。夜間は発声せず、昼寝中だけ使用します。</small>
</section>
<div class="save"><button type="submit">音声設定を保存</button><p id="voiceStatus" class="status" role="status">読み込み中…</p></div>
</form>
<section class="card"><h2>音声テスト（AIサーバー 12800）</h2>
<label for="testSpeechText">読み上げる文章</label><textarea id="testSpeechText" rows="2" placeholder="テスト発話の文章"></textarea>
<div class="buttons"><button type="button" id="testSpeech">読み上げテスト</button></div>
<small>12800の既存Gemini Live音声・字幕経路を使用。通常会話中は開始しません。</small>
<p id="testSpeechStatus" class="status" role="status">待機中</p></section>
<section id="actions" class="card"><h2>サーボアクションの実機テスト</h2>
<small>現在位置から相対的に動作します。うなずき・首振りは開始位置へ戻り、ほかの動作は終了位置を維持します。可動域は自動で制限します。</small>
<div id="servoActions" class="buttons">
<button type="button" data-action="nod">うなずく</button>
<button type="button" data-action="shake_head">首を横に振る</button>
<button type="button" data-action="tilt_head">首を傾ける</button>
<button type="button" data-action="happy_move">喜ぶ</button>
<button type="button" data-action="surprise_move">驚く</button>
<button type="button" data-action="shy_move">照れる</button>
</div><p id="actionStatus" class="status" role="status">待機中</p></section>
</main></div>
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
document.getElementById('servoActions').addEventListener('click',async e=>{
 const button=e.target.closest('button[data-action]');
 if(!button)return;
 const status=document.getElementById('actionStatus');
 button.disabled=true;status.textContent='要求中…';
 try{
  const res=await fetch('/api/stage1/action',{method:'POST',
   headers:{'Content-Type':'application/json'},
   body:JSON.stringify({action:button.dataset.action})});
  if(!res.ok)throw Error(res.status===409?'動作中または利用できません':'HTTP '+res.status);
  status.textContent='動作要求を受け付けました。';
 }catch(e){status.textContent='失敗: '+e.message;}
 finally{button.disabled=false;}
});
const voiceForm=document.getElementById('voiceSettings'),v=voiceForm.elements;
const voiceStatus=document.getElementById('voiceStatus'),voiceFields=['morning','bedtime','touch','wake_word','timer','snore','sleep_talk_prompt','snore_min','snore_max','talk_min','talk_max'];
async function loadVoice(){
 try{
  const r=await fetch('/api/stage1/voice',{cache:'no-store'});
  if(!r.ok)throw Error('HTTP '+r.status);
  const data=await r.json();for(const field of voiceFields)v[field].value=data[field]??'';
  voiceStatus.textContent='読み込み完了';
 }catch(e){voiceStatus.textContent='読み込み失敗: '+e.message;}
}
voiceForm.addEventListener('submit',async e=>{
 e.preventDefault();if(!voiceForm.reportValidity())return;
 for(const [low,high] of [['snore_min','snore_max'],['talk_min','talk_max']]){
  if(Number(v[low].value)>Number(v[high].value)){voiceStatus.textContent='最短間隔は最長以下にしてください';return;}
 }
 const enc=new TextEncoder();
 for(const field of ['morning','bedtime','touch','wake_word','timer','snore','sleep_talk_prompt']){
  const max=(field==='snore'||field==='sleep_talk_prompt')?256:512;
  if(enc.encode(v[field].value).length>max){voiceStatus.textContent=field+'は'+max+'バイト以内にしてください';return;}
 }
 const data={};for(const field of voiceFields)data[field]=
  ['snore_min','snore_max','talk_min','talk_max'].includes(field)?Number(v[field].value):v[field].value;
 voiceStatus.textContent='保存中…';
 try{
  const r=await fetch('/api/stage1/voice',{method:'POST',
   headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});
  if(!r.ok)throw Error('HTTP '+r.status+': '+await r.text());
  voiceStatus.textContent='保存して反映しました';
 }catch(error){voiceStatus.textContent='保存失敗: '+error.message;}
});
const testSpeech=document.getElementById('testSpeech');
testSpeech.addEventListener('click',async()=>{
 const message=document.getElementById('testSpeechStatus');
 const value=document.getElementById('testSpeechText').value;
 const size=new TextEncoder().encode(value).length;
 if(!size||size>512){message.textContent='文章はUTF-8で1～512バイトにしてください';return;}
 testSpeech.disabled=true;message.textContent='発話を要求中…';
 try{
  const res=await fetch('/api/stage1/voice/test',{method:'POST',
   headers:{'Content-Type':'application/json'},body:JSON.stringify({text:value})});
  if(!res.ok)throw Error('HTTP '+res.status+': '+await res.text());
  message.textContent='音声要求を受け付けました。再生結果は本体ログで確認してください。';
 }catch(e){message.textContent='発話できません: '+e.message;}
 finally{testSpeech.disabled=false;}
});
load();loadVoice();
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
// Voice text settings are deliberately separate from the small 512-byte
// scheduler request. Temporary objects live in PSRAM to protect internal RAM.
bool Uint(const cJSON* root, const char* key, int min, int max, int step, int& value);
esp_err_t ReadVoice(httpd_req_t* req) {
    cJSON* json = cJSON_CreateObject();
    if (!json) { Reply(req, "503 Service Unavailable", "{\"error\":\"low memory\"}"); return ESP_OK; }
    {
        LvglLockGuard lock;
        const auto* c = voice::Current();
        if (c) {
            cJSON_AddStringToObject(json, "morning", c->morning);
            cJSON_AddStringToObject(json, "bedtime", c->bedtime);
            cJSON_AddStringToObject(json, "touch", c->touch);
            cJSON_AddStringToObject(json, "wake_word", c->wake_word);
            cJSON_AddStringToObject(json, "timer", c->timer);
            cJSON_AddStringToObject(json, "snore", c->snore);
            cJSON_AddStringToObject(json, "sleep_talk_prompt", c->sleep_talk_prompt);
            cJSON_AddNumberToObject(json, "snore_min", c->snore_min);
            cJSON_AddNumberToObject(json, "snore_max", c->snore_max);
            cJSON_AddNumberToObject(json, "talk_min", c->talk_min);
            cJSON_AddNumberToObject(json, "talk_max", c->talk_max);
        }
    }
    char* text = cJSON_PrintUnformatted(json);
    const bool filled = cJSON_GetObjectItemCaseSensitive(json, "morning") != nullptr;
    cJSON_Delete(json);
    if (!filled || !text) {
        if (text) cJSON_free(text);
        Reply(req, "503 Service Unavailable", "{\"error\":\"voice settings unavailable\"}");
        return ESP_OK;
    }
    Reply(req, "200 OK", text);
    cJSON_free(text);
    return ESP_OK;
}
bool VoiceText(const cJSON* json, const char* name, char* dest, size_t capacity) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(json, name);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    const size_t len = std::strlen(item->valuestring);
    if (len >= capacity) return false;
    std::memcpy(dest, item->valuestring, len + 1);
    return true;
}
esp_err_t WriteVoice(httpd_req_t* req) {
    char type[64]{};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        std::strncmp(type, "application/json", 16) != 0) {
        Reply(req,"415 Unsupported Media Type","{\"error\":\"JSON required\"}"); return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > 8192) {
        Reply(req,"413 Payload Too Large","{\"error\":\"invalid size\"}"); return ESP_OK;
    }
    char* buf = static_cast<char*>(heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    void* memory = heap_caps_malloc(sizeof(voice::Settings), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf || !memory) {
        if (buf) heap_caps_free(buf);
        if (memory) heap_caps_free(memory);
        Reply(req,"503 Service Unavailable","{\"error\":\"PSRAM unavailable\"}"); return ESP_OK;
    }
    auto* draft = new(memory) voice::Settings();
    int got = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { heap_caps_free(memory); heap_caps_free(buf); return ESP_FAIL; }
        got += n;
    }
    buf[got] = 0;
    cJSON* root = cJSON_Parse(buf);
    heap_caps_free(buf);
    const auto* c = root;
    int snore_lo=0, snore_hi=0, talk_lo=0, talk_hi=0;
    const bool valid = cJSON_IsObject(c) &&
        VoiceText(c,"morning",draft->morning,sizeof(draft->morning)) &&
        VoiceText(c,"bedtime",draft->bedtime,sizeof(draft->bedtime)) &&
        VoiceText(c,"touch",draft->touch,sizeof(draft->touch)) &&
        VoiceText(c,"wake_word",draft->wake_word,sizeof(draft->wake_word)) &&
        VoiceText(c,"timer",draft->timer,sizeof(draft->timer)) &&
        VoiceText(c,"snore",draft->snore,sizeof(draft->snore)) &&
        VoiceText(c,"sleep_talk_prompt",draft->sleep_talk_prompt,sizeof(draft->sleep_talk_prompt)) &&
        Uint(c,"snore_min",1,3600,1,snore_lo) && Uint(c,"snore_max",1,3600,1,snore_hi) &&
        Uint(c,"talk_min",1,3600,1,talk_lo) && Uint(c,"talk_max",1,3600,1,talk_hi);
    draft->snore_min=snore_lo; draft->snore_max=snore_hi;
    draft->talk_min=talk_lo; draft->talk_max=talk_hi;
    bool saved=false;
    if (valid && voice::Valid(*draft)) {
        LvglLockGuard lock;
        saved=voice::Save(*draft);
    }
    cJSON_Delete(root);
    heap_caps_free(memory);
    Reply(req, !valid?"400 Bad Request":!saved?"500 Internal Server Error":"200 OK",
        !valid?"{\"error\":\"invalid voice settings\"}":
        !saved?"{\"error\":\"NVS save failed\"}":"{\"ok\":true}");
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

esp_err_t TriggerAction(httpd_req_t* req) {
    char content_type[64]{};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        std::strncmp(content_type, "application/json", 16) != 0) {
        Reply(req, "415 Unsupported Media Type", "{\"error\":\"JSON required\"}");
        return ESP_OK;
    }
    if (req->content_len < 1 || req->content_len > 96) {
        Reply(req, "400 Bad Request", "{\"error\":\"invalid action request\"}");
        return ESP_OK;
    }
    char body[97]{};
    int read = 0;
    while (read < req->content_len) {
        const int n = httpd_req_recv(req, body + read, req->content_len - read);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return ESP_FAIL;
        read += n;
    }
    cJSON* request = cJSON_Parse(body);
    const cJSON* action = request ? cJSON_GetObjectItemCaseSensitive(request, "action") : nullptr;
    const bool accepted = cJSON_IsString(action) && action->valuestring &&
        std::strlen(action->valuestring) <= 32 && RequestCue(action->valuestring);
    cJSON_Delete(request);
    Reply(req, accepted ? "202 Accepted" : "409 Conflict",
          accepted ? "{\"queued\":true}" : "{\"error\":\"unknown action or busy\"}");
    return ESP_OK;
}

esp_err_t TriggerVoiceTest(httpd_req_t* req) {
    char type[64]{};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        std::strncmp(type, "application/json", 16) != 0) {
        Reply(req, "415 Unsupported Media Type", "{\"error\":\"JSON required\"}");
        return ESP_OK;
    }
    if (req->content_len < 1 || req->content_len > 2048) {
        Reply(req, "413 Payload Too Large", "{\"error\":\"invalid test size\"}");
        return ESP_OK;
    }
    char* body = static_cast<char*>(heap_caps_malloc(
        req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!body) {
        Reply(req, "503 Service Unavailable", "{\"error\":\"PSRAM unavailable\"}");
        return ESP_OK;
    }
    int received = 0;
    while (received < req->content_len) {
        const int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { heap_caps_free(body); return ESP_FAIL; }
        received += n;
    }
    body[received] = '\0';
    cJSON* root = cJSON_ParseWithLength(body, received);
    const cJSON* item = root ? cJSON_GetObjectItemCaseSensitive(root, "text") : nullptr;
    const char* content = cJSON_IsString(item) ? item->valuestring : nullptr;
    const size_t len = content ? std::strlen(content) : 0;
    const bool valid = content && len >= 1 && len <= 512;
    bool accepted = false;
    if (valid) {
        LvglLockGuard lock;
        const auto* config = voice::Current();
        if (config) accepted = voice::Request(*config, "test", content);
    }
    cJSON_Delete(root);
    heap_caps_free(body);
    Reply(req, !valid ? "400 Bad Request" :
          accepted ? "202 Accepted" : "409 Conflict",
          !valid ? "{\"error\":\"text must be 1 to 512 bytes\"}" :
          accepted ? "{\"queued\":true}" :
          "{\"error\":\"speech busy or unavailable\"}");
    return ESP_OK;
}

void Start() {
    // Called from the StackChan update task after boot, only when Xiaozhi is
    // idle. Low heap at the first attempt must not disable settings forever.
    if (attempted.load()) return;
    static uint32_t last_attempt_ms = 0;
    const uint32_t now = GetHAL().millis();
    if (last_attempt_ms && uint32_t(now - last_attempt_ms) < 30000) return;
    last_attempt_ms = now;
    const size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "deferred start: internal_free=%u largest=%u",
             static_cast<unsigned>(free_bytes), static_cast<unsigned>(largest));
    if (free_bytes < MIN_FREE_INTERNAL || largest < MIN_LARGEST_INTERNAL) {
        ESP_LOGW(TAG, "web start deferred: insufficient internal heap; will retry while idle");
        return;
    }
    if (attempted.exchange(true)) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 4096;
    config.max_uri_handlers = 9;
    config.max_open_sockets = 4;
    const esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "port 80 start failed: %s", esp_err_to_name(err));
        server = nullptr;
        attempted.store(false);
        return;
    }
    httpd_uri_t page{};
    page.uri = "/"; page.method = HTTP_GET; page.handler = Page;
    httpd_uri_t read{};
    read.uri = "/api/stage1"; read.method = HTTP_GET; read.handler = ReadConfig;
    httpd_uri_t write{};
    write.uri = "/api/stage1"; write.method = HTTP_POST; write.handler = WriteConfig;
    httpd_uri_t action{};
    action.uri = "/api/stage1/action"; action.method = HTTP_POST; action.handler = TriggerAction;
    httpd_uri_t voice_read{};
    voice_read.uri = "/api/stage1/voice"; voice_read.method = HTTP_GET; voice_read.handler = ReadVoice;
    httpd_uri_t voice_write{};
    voice_write.uri = "/api/stage1/voice"; voice_write.method = HTTP_POST; voice_write.handler = WriteVoice;
    httpd_uri_t voice_test{};
    voice_test.uri = "/api/stage1/voice/test"; voice_test.method = HTTP_POST; voice_test.handler = TriggerVoiceTest;
    httpd_uri_t wifi_read{};
    wifi_read.uri = "/api/stage1/wifi"; wifi_read.method = HTTP_GET;
    wifi_read.handler = stackchan::wifi::web::ReadSettings;
    httpd_uri_t wifi_write{};
    wifi_write.uri = "/api/stage1/wifi"; wifi_write.method = HTTP_POST;
    wifi_write.handler = stackchan::wifi::web::WriteSettings;
    if (httpd_register_uri_handler(server, &page) != ESP_OK ||
        httpd_register_uri_handler(server, &read) != ESP_OK ||
        httpd_register_uri_handler(server, &write) != ESP_OK ||
        httpd_register_uri_handler(server, &action) != ESP_OK ||
        httpd_register_uri_handler(server, &voice_read) != ESP_OK ||
        httpd_register_uri_handler(server, &voice_write) != ESP_OK ||
        httpd_register_uri_handler(server, &voice_test) != ESP_OK ||
        httpd_register_uri_handler(server, &wifi_read) != ESP_OK ||
        httpd_register_uri_handler(server, &wifi_write) != ESP_OK) {
        ESP_LOGE(TAG, "URI registration failed");
        httpd_stop(server);
        server = nullptr;
        attempted.store(false);
        return;
    }
    ESP_LOGI(TAG, "ready: port=80 internal_free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}
}
