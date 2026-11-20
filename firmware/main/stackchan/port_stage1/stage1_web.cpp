// Local Stage-1 settings page; independent of the Xiaozhi WebSocket.
#include "stage1_web.h"
#include "port_stage1.h"
#include "voice_settings.h"
#include "voice_client.h"
#include "quiet_mode.h"
#include "environment.h"
#include "setup_web.h"
#include "stackchan/wifi/wifi_web.h"
#include <hal/hal.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <atomic>
#include <cJSON.h>
#include <cstdio>
#include <cstring>
#include <new>

extern "C" bool stackchan_sd_nvs_backup_pending(void);

namespace stackchan::stage1::web {
namespace {
constexpr const char* TAG = "STAGE1-WEB";
httpd_handle_t server = nullptr;
std::atomic<bool> attempted{false};
// HTTPD task stack is allocated in PSRAM; retain headroom for lwIP and kernel objects.
constexpr size_t MIN_FREE_INTERNAL = 12000;
constexpr size_t MIN_FREE_PSRAM = 16384;
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
input[type=time],input[type=number],input[type=text],input[type=password],textarea{display:block;width:100%;max-width:14rem;min-height:30px;border:1px solid #bac5d6;border-radius:7px;background:#fff;color:inherit;padding:5px 8px}
textarea{display:block;resize:vertical;min-height:60px;width:100%;max-width:none}
.check{display:flex;gap:8px;align-items:center;margin:0 0 12px}
input[type=checkbox]{width:16px;height:16px;margin:0}
button{min-height:30px;padding:5px 12px;background:#285bd4;color:white;border:0;border-radius:7px;cursor:pointer}
button:hover{background:#204bb4}button:disabled{opacity:.45;cursor:default}
.danger{background:#9b3540}.danger:hover{background:#7f2b34}
.setup-row{display:flex;align-items:end;gap:10px;flex-wrap:wrap;margin:8px 0 12px}
.setup-row label{flex:1 1 190px;min-width:0}
.setup-row select{display:block;width:100%;max-width:19rem;min-height:30px;border:1px solid #bac5d6;border-radius:7px;background:white;color:inherit;padding:5px 8px}
.setup-ro{display:grid;grid-template-columns:minmax(105px,170px) minmax(0,1fr);gap:7px;margin:7px 0}
.setup-ro b{font-weight:500}.setup-ro span{overflow-wrap:anywhere}
@media(max-width:440px){.setup-ro{grid-template-columns:1fr;gap:0}}
.wifi-network{border:1px solid #d6dce7;border-radius:8px;padding:10px;margin:10px 0}
.wifi-network .wifi-static{margin-top:10px}
.wifi-network select,.wifi-network input[type=password]{display:block;width:100%;max-width:16rem;min-height:30px;border:1px solid #bac5d6;border-radius:7px;padding:5px 8px}
.wifi-network .wifi-action{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}
.wifi-network .wifi-delete{background:#9b3540}
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
<a href="#daily">起床・就寝</a><a href="#theatre">小芝居</a><a href="#nap">昼寝</a><a href="#voice">音声・文言</a><a href="#quiet">お黙り</a><a href="#environment">天気・位置</a><a href="#actions">サーボテスト</a><a href="#wifi">Wi-Fi</a><a href="#device">デバイス</a><a href="#aiSetup">AI・省電力</a><a href="#appsSetup">アプリ表示</a><a href="#deviceInfo">端末情報</a>
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
<section class="card"><h2>独り言</h2>
<label class="check"><input name="monologue_enabled" type="checkbox"> 起床中の独り言を有効にする</label>
<label>独り言への指示<textarea name="monologue_prompt" rows="3"></textarea></label>
<div class="fields">
<label>最短間隔（分）<input name="monologue_min" type="number" min="1" max="1440" required></label>
<label>最長間隔（分）<input name="monologue_max" type="number" min="1" max="1440" required></label>
</div><small>起床・待機中だけ、既存のAIサーバーへ生成を依頼します。会話・睡眠・静音中は開始しません。</small>
<div class="buttons"><button type="button" id="testMonologue">独り言を今すぐテスト</button></div>
<p id="testMonologueStatus" class="status" role="status">待機中</p>
</section>
<div class="save"><button type="submit">音声設定を保存</button><p id="voiceStatus" class="status" role="status">読み込み中…</p></div>
</form>
<section class="card"><h2>音声テスト（AIサーバー 12800）</h2>
<label for="testSpeechText">読み上げる文章</label><textarea id="testSpeechText" rows="2" placeholder="テスト発話の文章"></textarea>
<div class="buttons"><button type="button" id="testSpeech">読み上げテスト</button></div>
<small>12800の既存Gemini Live音声・字幕経路を使用。通常会話中は開始しません。</small>
<p id="testSpeechStatus" class="status" role="status">待機中</p></section>
<section id="quiet" class="card"><h2>お黙りモード</h2>
<p id="quietStatus" class="status" role="status">状態を読み込み中…</p>
<div class="buttons"><button type="button" class="danger" id="quietCancel" disabled>今回のお黙りモードを解除</button></div>
<small>独り言・いびき・寝言・昼寝からの自動起床時の発話・夢の感想を停止します。起床・就寝時の挨拶、ウェイクワード、手動会話、Webテスト、無音の小芝居は従来どおり。待受中は口に白いテープを表示します。</small>
<h3>今から静かにする</h3>
<div class="setup-row"><label>時間（分）<input id="quietMinutes" type="number" min="1" max="1440" value="30"></label>
<button type="button" id="quietApply" disabled>この時間だけ静かにする</button></div>
<small>1～1440分。時間を省略した「静かにして」「黙って」は30分。</small>
<h3>指定時刻まで</h3>
<div class="setup-row"><label>終了時刻<input id="quietUntil" type="time" value="15:00"></label>
<button type="button" id="quietApplyUntil" disabled>この時刻まで静かにする</button></div>
<small>すでに過ぎた時刻は翌日まで。</small>
<h3>時間帯の予約</h3>
<label class="check"><input id="quietEnabled" type="checkbox">時間帯の予約を使用</label>
<label class="check"><input id="quietDaily" type="checkbox" checked>毎日繰り返す</label>
<div class="fields"><label>開始時刻<input id="quietStart" type="time" value="12:00"></label>
<label>終了時刻<input id="quietEnd" type="time" value="13:00"></label></div>
<div class="buttons"><button type="button" id="quietSave" disabled>保存して反映</button></div>
<small>日をまたぐ予約に対応。毎日をオフにすると次の該当時間帯の一回だけ。今回だけ解除しても翌日の予約は維持します。一時指定と予約が重なる場合は遅い終了時刻まで有効です。再起動後も保存設定を使用します。</small>
<p id="quietActionStatus" class="status" role="status"></p>
</section>
<section id="environment" class="card"><h2>天気・位置情報</h2>
<p id="environmentInfo" class="status">位置情報を読み込み中…</p>
<div class="fields">
<label>地域の表示名（任意）<input id="environmentRegion" type="text" maxlength="127" autocomplete="off" placeholder="例：東京都 千代田区"></label>
<label>OpenWeatherMap APIキー<input id="environmentKey" type="password" maxlength="128" autocomplete="new-password" placeholder="変更する場合のみ入力"></label>
</div>
<label class="check"><input id="environmentClearKey" type="checkbox">登録済みAPIキーを削除する</label>
<div class="buttons"><button type="button" id="environmentSave" disabled>保存して取得</button><button type="button" id="environmentReload">状態を再読込</button></div>
<p id="environmentStatus" class="status" role="status">読み込み中…</p>
<small>接続元IPから地域を推定し、OpenWeatherMapの現在天気を取得します。APIキーは画面へ返さずNVSに保存し、SDのconfig.jsonへ非同期バックアップします。バックアップにはAPIキーも含まれるため、SDカードの取り扱いに注意してください。天気は約10分、位置は約1時間ごとに、会話していないときだけ更新します。取得から30分を超えた天気は装飾に使いません。</small>
</section>
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
<section id="wifi" class="card"><h2>Wi-Fi接続設定</h2>
<p id="wifiCurrent" class="status">接続情報を読み込み中…</p>
<small>SSIDごとにDHCP／固定IPを選択。既存パスワードは表示しません。空欄では変更されません。
新しいSSIDを登録しても旧SSIDは復旧用に残ります。現在接続中のSSIDと最後の1件は削除できません。</small>
<div id="wifiNetworks"></div>
<div class="buttons"><button type="button" id="wifiAdd">SSIDを追加</button>
<button type="button" id="wifiReload">再読み込み</button></div>
<p id="wifiStatus" class="status" role="status">読み込み中…</p>
<small>保存先はNVS、SDバックアップは既存の非同期処理で反映します。
接続中のSSIDを保存した場合はWi-Fiを再接続します。
Webとの接続が切れた場合は新しいIPアドレスから開き直してください。</small>
</section>
<section id="device" class="card"><h2>デバイス設定</h2>
<div id="setupDeviceFields"></div>
<small>明るさ・音量は本体と同じ値を保存して反映します。タイムゾーンも本体SETUPと同じ選択肢です。</small></section>
<section id="aiSetup" class="card"><h2>AI設定</h2>
<div id="setupAiFields"></div>
<h3>接続先AIサーバー</h3>
<div class="setup-row"><label>サーバーURL
<input id="setupServerUrl" type="url" maxlength="192" autocomplete="off"
 placeholder="http://192.168.1.254:12800" required></label>
<button id="setupServerSave" type="button" disabled>保存</button></div>
<small>サーバーの基底URLを指定します。保存時にOTA設定URLと会話WebSocket URLを同じ接続先へ更新します。
パスは入力せず、例のようにスキーム・ホスト・ポートだけを指定してください。認証トークンは変更しません。
反映には本体の再起動が必要です。</small>
<p id="setupServerStatus" class="status"></p>
<small>本体SETUPの「General」と共通の設定値です。保存後は次回の本体起動時に確実に適用されます。</small></section>
<section id="appsSetup" class="card"><h2>アプリの表示・非表示</h2>
<div id="setupAppFields"></div>
<small>ランチャーのアイコン表示だけを変更します。AI自動起動とは別設定です。SETUPは非表示になりません。</small></section>
<section id="deviceInfo" class="card"><h2>アカウント・端末情報（閲覧のみ）</h2>
<div id="setupInfoRows"></div>
<small>アカウント名と端末名は本体保存済み情報を表示します。ここからサーバーへの再取得、解除、初期化、OTA更新は行いません。</small>
<h2>ハードウェア・診断</h2>
<p>サーボの動作確認は上の「サーボテスト」から実行できます。
マイク・RGBストリップのテスト、Wi-Fi接続テストは本体のSETUPアプリで行えます。</p></section>
<section class="card"><h2>本体SETUPの読込・保存状態</h2>
<div class="buttons"><button id="setupReload" type="button">設定を再読み込み</button></div>
<p id="setupStatus" class="status" role="status">読み込み中…</p>
<small>NVSへの保存とSDバックアップの完了は別々です。SD同期が保留の場合はその状態を表示します。</small>
</section>
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
 const voiceStatus=document.getElementById('voiceStatus'),voiceFields=['morning','bedtime','touch','wake_word','timer','snore','sleep_talk_prompt','snore_min','snore_max','talk_min','talk_max','monologue_enabled','monologue_prompt','monologue_min','monologue_max'];
async function loadVoice(){
 try{
  const r=await fetch('/api/stage1/voice',{cache:'no-store'});
  if(!r.ok)throw Error('HTTP '+r.status);
   const data=await r.json();for(const field of voiceFields){
    if(field==='monologue_enabled')v[field].checked=!!data[field];else v[field].value=data[field]??'';
   }
  voiceStatus.textContent='読み込み完了';
 }catch(e){voiceStatus.textContent='読み込み失敗: '+e.message;}
}
voiceForm.addEventListener('submit',async e=>{
 e.preventDefault();if(!voiceForm.reportValidity())return;
  for(const [low,high] of [['snore_min','snore_max'],['talk_min','talk_max'],['monologue_min','monologue_max']]){
  if(Number(v[low].value)>Number(v[high].value)){voiceStatus.textContent='最短間隔は最長以下にしてください';return;}
 }
 const enc=new TextEncoder();
  for(const field of ['morning','bedtime','touch','wake_word','timer','snore','sleep_talk_prompt','monologue_prompt']){
   const max=(field==='snore'||field==='sleep_talk_prompt')?256:512;
  if(enc.encode(v[field].value).length>max){voiceStatus.textContent=field+'は'+max+'バイト以内にしてください';return;}
 }
  const data={};for(const field of voiceFields)data[field]=field==='monologue_enabled'?v[field].checked:
   ['snore_min','snore_max','talk_min','talk_max','monologue_min','monologue_max'].includes(field)?Number(v[field].value):v[field].value;
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
const testMonologue=document.getElementById('testMonologue');
testMonologue.addEventListener('click',async()=>{
 const message=document.getElementById('testMonologueStatus'),value=v.monologue_prompt.value;
 const size=new TextEncoder().encode(value).length;
 if(!size||size>512){message.textContent='独り言への指示はUTF-8で1～512バイトにしてください';return;}
 testMonologue.disabled=true;message.textContent='要求中…';
 try{
  const res=await fetch('/api/stage1/voice/test',{method:'POST',headers:{'Content-Type':'application/json'},
   body:JSON.stringify({kind:'monologue',text:value})});
  if(!res.ok)throw Error(res.status===409?'会話中または発話中です':'HTTP '+res.status);
  message.textContent='独り言を受け付けました';
 }catch(e){message.textContent='失敗: '+e.message;}
 finally{testMonologue.disabled=false;}
});
const quietStatus=document.getElementById('quietStatus'), q=id=>document.getElementById(id);
let quietLoaded=false,quietPending=false,quietData=null;
function quietButtons(){
 for(const id of ['quietApply','quietApplyUntil','quietSave','quietCancel'])
  q(id).disabled=!quietLoaded||quietPending||(id==='quietCancel'&&!quietData.active);
}
function renderQuiet(data,fill){
 quietData=data;quietLoaded=true;
 if(fill){q('quietEnabled').checked=data.settings.enabled;q('quietDaily').checked=data.settings.daily;q('quietStart').value=data.settings.start_time;q('quietEnd').value=data.settings.end_time;}
 quietStatus.textContent=!data.clock_ready?'本体の時刻同期を待っています。':data.active?'お黙りモード中 ／ '+new Date(data.until*1000).toLocaleString('ja-JP')+' まで（残り約'+Math.max(1,Math.ceil(data.remaining_seconds/60))+'分）':'通常動作中'+(data.settings.enabled?' ／ '+(data.settings.daily?'毎日予約あり':data.settings.once_end>data.now?'一回予約あり':'一回予約終了'):'');
 quietButtons();
}
async function loadQuiet(){
 if(quietPending)return;
 try{const res=await fetch('/api/stage1/quiet',{cache:'no-store'});if(!res.ok)throw Error('HTTP '+res.status);const data=await res.json();if(!quietPending)renderQuiet(data,!quietLoaded);}
 catch(e){quietStatus.textContent='状態を取得できません: '+e.message;}
}
async function setQuiet(body){
 if(!quietLoaded||quietPending)return;
 quietPending=true;quietButtons();q('quietActionStatus').textContent='保存して反映中…';
 try{
  const res=await fetch('/api/stage1/quiet',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  if(!res.ok)throw Error(await res.text());
  renderQuiet(await res.json(),body.action==='schedule');
  q('quietActionStatus').textContent=body.action==='cancel'?'今回のお黙りモードを解除しました。':'保存して反映しました。';
 }catch(e){q('quietActionStatus').textContent='保存失敗: '+e.message;}
 finally{quietPending=false;quietButtons();}
}
q('quietCancel').addEventListener('click',()=>setQuiet({action:'cancel'}));
q('quietApply').addEventListener('click',()=>{const n=Number(q('quietMinutes').value);if(!Number.isInteger(n)||n<1||n>1440){q('quietActionStatus').textContent='時間は1～1440分で指定してください。';return;}setQuiet({action:'minutes',minutes:n});});
q('quietApplyUntil').addEventListener('click',()=>{if(q('quietUntil').value)setQuiet({action:'until',end_time:q('quietUntil').value});});
q('quietSave').addEventListener('click',()=>{const start=q('quietStart').value,end=q('quietEnd').value;if(!start||!end||start===end){q('quietActionStatus').textContent='開始と終了は異なる時刻を指定してください。';return;}setQuiet({action:'schedule',enabled:q('quietEnabled').checked,daily:q('quietDaily').checked,start_time:start,end_time:end});});
setInterval(loadQuiet,10000);
const environmentInfo=document.getElementById('environmentInfo');
const environmentStatus=document.getElementById('environmentStatus');
const environmentRegion=document.getElementById('environmentRegion');
const environmentKey=document.getElementById('environmentKey');
const environmentClearKey=document.getElementById('environmentClearKey');
const environmentSave=document.getElementById('environmentSave');
let environmentLoaded=false,environmentPending=false;
async function loadEnvironment(){
 try{
  const response=await fetch('/api/stage1/environment',{cache:'no-store'});
  if(!response.ok)throw Error('HTTP '+response.status+': '+await response.text());
  const d=await response.json();
  if(!environmentLoaded){environmentRegion.value=d.region_override||'';environmentLoaded=true;environmentSave.disabled=false;}
  const place=d.location_valid?(d.region_name||d.city||'取得済み')+' ('+d.latitude.toFixed(4)+', '+d.longitude.toFixed(4)+')':'位置情報は未取得です';
  const weather=d.weather_valid?(d.weather_stale?'前回の天気: ':'現在の天気: ')+d.description+' / '+d.temperature_c.toFixed(1)+'℃ / 湿度'+d.humidity_percent+'% / '+d.weather_age_minutes+'分前':'天気は未取得です';
  environmentInfo.textContent=place+'\n'+weather;
  environmentStatus.textContent=(d.weather_key_set?'APIキー登録済み':'APIキー未登録')+' / 地域HTTP '+d.location_http+' / 天気HTTP '+d.weather_http+(d.fetching?' / 取得中':'')+(d.sd_pending?' / SD同期待ち':'');
 }catch(e){environmentStatus.textContent='読込失敗: '+e.message;}
}
environmentSave.addEventListener('click',async()=>{
 if(!environmentLoaded||environmentPending)return;
 const encoder=new TextEncoder();
 if(encoder.encode(environmentRegion.value.trim()).length>127||encoder.encode(environmentKey.value.trim()).length>128){environmentStatus.textContent='地域名は127バイト、APIキーは128バイト以内です';return;}
 environmentPending=true;environmentSave.disabled=true;environmentStatus.textContent='NVSへ保存中…';
 try{
  const response=await fetch('/api/stage1/environment',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({region_name:environmentRegion.value.trim(),weather_api_key:environmentKey.value.trim(),clear_key:environmentClearKey.checked})});
  if(!response.ok)throw Error('HTTP '+response.status+': '+await response.text());
  environmentKey.value='';environmentClearKey.checked=false;await loadEnvironment();
  environmentStatus.textContent='NVSへ保存しました。位置・天気は待機中に取得します。SDバックアップは非同期です。';
 }catch(e){environmentStatus.textContent='保存失敗: '+e.message;}
 finally{environmentPending=false;environmentSave.disabled=false;}
});
document.getElementById('environmentReload').addEventListener('click',loadEnvironment);
const wifiList=document.getElementById('wifiNetworks'),wifiStatus=document.getElementById('wifiStatus');
let wifiSaved=[],wifiActive='';
const ipFields=[['ip','IPアドレス'],['subnet','サブネットマスク'],['gw','ゲートウェイ'],['dns1','DNS1'],['dns2','DNS2（任意）']];
function wifiRow(data={}){
 const row=document.createElement('div');row.className='wifi-network';
 row.dataset.originalSsid=data.ssid||'';
 row.innerHTML='<div class="fields"><label>SSID<input class="wifi-ssid" type="text" maxlength="32" required></label>'+
 '<label>パスワード<input class="wifi-pass" type="password" maxlength="63" autocomplete="new-password" placeholder="変更する場合のみ入力"></label></div>'+
 '<label class="check"><input class="wifi-clear" type="checkbox">パスワードを空にする（オープンWi-Fi）</label>'+
 '<label>IPアドレスの取得方法<select class="wifi-mode"><option value="dhcp">DHCP（自動取得）</option><option value="static">Static（固定IP）</option></select></label>'+
 '<div class="wifi-static fields" hidden></div>'+
 '<label class="check"><input class="wifi-reconnect" type="checkbox">保存後、Wi-Fiへ再接続</label>'+
 '<div class="wifi-action"><button class="wifi-save" type="button">このSSIDを保存</button><button class="wifi-delete" type="button">削除</button></div>';
 row.querySelector('.wifi-ssid').value=data.ssid||'';
 row.querySelector('.wifi-mode').value=data.mode==='static'?'static':'dhcp';
 row.querySelector('.wifi-reconnect').checked=!data.ssid||data.ssid===wifiActive;
 row.querySelector('.wifi-delete').disabled=!data.ssid||data.ssid===wifiActive||wifiSaved.length<=1;
 const region=row.querySelector('.wifi-static');
 for(const [key,label] of ipFields){
   const wrapper=document.createElement('label'),field=document.createElement('input');
   wrapper.textContent=label;field.type='text';field.className='wifi-'+key;field.maxLength=15;
   field.placeholder=key==='subnet'?'255.255.255.0':(key==='dns2'?'省略可':'192.168.1.1');
   field.value=data[key]||'';wrapper.append(field);region.append(wrapper);
 }
 const toggle=()=>{
   const fixed=row.querySelector('.wifi-mode').value==='static';
   region.hidden=!fixed;
   for(const [key] of ipFields){
     const field=row.querySelector('.wifi-'+key);
     field.required=fixed&&key!=='dns2';field.disabled=!fixed;
   }
 };
 row.querySelector('.wifi-mode').addEventListener('change',toggle);toggle();
 row.querySelector('.wifi-clear').addEventListener('change',()=>{
   row.querySelector('.wifi-pass').disabled=row.querySelector('.wifi-clear').checked;
 });
 row.querySelector('.wifi-save').addEventListener('click',async()=>{
   const ssid=row.querySelector('.wifi-ssid').value.trim();
   if(!ssid){wifiStatus.textContent='SSIDを入力してください';return;}
   const encoder=new TextEncoder();
   if(encoder.encode(ssid).length>32||encoder.encode(row.querySelector('.wifi-pass').value).length>63){
     wifiStatus.textContent='SSIDは32バイト、パスワードは63バイト以内です';return;
   }
   if(!row.querySelector('.wifi-ssid').reportValidity())return;
   const payload={action:'save',ssid:ssid,password:row.querySelector('.wifi-pass').value,
    clear_password:row.querySelector('.wifi-clear').checked,
    mode:row.querySelector('.wifi-mode').value,
    reconnect:row.querySelector('.wifi-reconnect').checked};
   if(payload.mode==='static'){
     for(const [key] of ipFields){
       const field=row.querySelector('.wifi-'+key);
       if(!field.reportValidity())return;
       const value=field.value.trim();
       if(key==='dns2'&&!value)continue;
       if(!/^([0-9]{1,3}\.){3}[0-9]{1,3}$/.test(value)||
         value.split('.').some(part=>Number(part)>255)){
         wifiStatus.textContent=key+'には正しいIPv4アドレスを指定してください';return;
       }
       payload[key]=value;
     }
   }
   const renamed=!!row.dataset.originalSsid&&row.dataset.originalSsid!==ssid;
   await wifiRequest(payload);
   if(renamed)wifiStatus.textContent+=' 旧SSIDは復旧用に残しました。';
 });
 row.querySelector('.wifi-delete').addEventListener('click',async()=>{
   if(!row.dataset.originalSsid||row.dataset.originalSsid===wifiActive)return;
   if(!confirm('SSID「'+row.dataset.originalSsid+'」を削除しますか？'))return;
   await wifiRequest({action:'delete',ssid:row.dataset.originalSsid});
 });
 wifiList.append(row);
}
async function wifiLoad(){
 wifiStatus.textContent='Wi-Fi設定を読み込み中…';
 try{
   const response=await fetch('/api/stage1/wifi',{cache:'no-store'});
   if(!response.ok)throw Error('HTTP '+response.status+': '+await response.text());
   const data=await response.json();
   wifiActive=data.connected?data.connected_ssid:'';
   wifiSaved=data.networks||[];
   document.getElementById('wifiCurrent').textContent=data.connected?
    '現在の接続: '+data.connected_ssid+' / IP: '+data.connected_ip:'Wi-Fiは未接続です';
   wifiList.replaceChildren();wifiSaved.forEach(wifiRow);
   wifiStatus.textContent=data.sd_pending?'読み込み完了 / SD同期待ち':'読み込み完了';
 }catch(err){wifiStatus.textContent='読み込み失敗: '+err.message;}
}
async function wifiRequest(payload){
 wifiStatus.textContent='NVSへ保存中…';
 try{
   const response=await fetch('/api/stage1/wifi',{method:'POST',
     headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});
   if(!response.ok)throw Error('HTTP '+response.status+': '+await response.text());
   const result=await response.json();
   await wifiLoad();
   wifiStatus.textContent=result.saved_nvs?
     'NVSへ保存済み。SDバックアップは同期待ち'+
      (result.reconnect?'。Wi-Fiを再接続します。':'。'):'保存結果を確認してください';
 }catch(err){wifiStatus.textContent='保存失敗: '+err.message;}
}
document.getElementById('wifiAdd').addEventListener('click',()=>{
 if(wifiList.querySelectorAll('.wifi-network').length>=10){
   wifiStatus.textContent='SSIDは最大10件です';return;
 }
 wifiRow({});wifiStatus.textContent='SSIDを入力して保存してください';
});
document.getElementById('wifiReload').addEventListener('click',wifiLoad);
// SETUP app mirror: use the same HAL and app-visibility persistence APIs.
const setupStatus=document.getElementById('setupStatus');
const setupTz=[
 ['BIT12','Baker Is. (UTC-12)'],['SST11','Midway Island (UTC-11)'],
 ['HST10','Honolulu (UTC-10)'],['AKST9','Alaska (UTC-9)'],
 ['PST8','Los Angeles (UTC-8)'],['MST7','Denver (UTC-7)'],
 ['CST6','Chicago (UTC-6)'],['EST5','New York (UTC-5)'],
 ['AST4','Halifax (UTC-4)'],['BRT3','S.Paulo (UTC-3)'],
 ['GST2','S.Georgia (UTC-2)'],['AZOT1','Azores (UTC-1)'],
 ['GMT0','London (UTC+0)'],['CET-1','Berlin (UTC+1)'],
 ['EET-2','Cairo (UTC+2)'],['MSK-3','Moscow (UTC+3)'],
 ['GST-4','Dubai (UTC+4)'],['PKT-5','Karachi (UTC+5)'],
 ['BST-6','Dhaka (UTC+6)'],['ICT-7','Bangkok (UTC+7)'],
 ['CST-8','Beijing (UTC+8)'],['JST-9','Tokyo (UTC+9)'],
 ['AEST-10','Sydney (UTC+10)'],['SBT-11','Noumea (UTC+11)'],
 ['NZST-12','Auckland (UTC+12)'],['FJT-13','Fiji (UTC+13)'],
 ['LINT-14','Line Islands (UTC+14)']];
const setupAppNames=[
 ['ai_agent','AI.AGENT'],['avatar','AVATAR'],['espnow_remote','ESPNOW.REMOTE'],
 ['app_center','APP.CENTER'],['ezdata','EZDATA'],['dance','DANCE']];
async function setupSave(field,value,app){
 setupStatus.textContent='NVSへ保存中: '+field;
 try{
  const payload={field,value};if(app)payload.app=app;
  const response=await fetch('/api/stage1/setup',{method:'POST',
   headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});
  if(!response.ok)throw Error('HTTP '+response.status+': '+await response.text());
  const result=await response.json();
  const loaded=await setupLoad();
  if(loaded)setupStatus.textContent='NVSに保存しました。SDバックアップは別途同期'+
   (result.reboot_required?'。AI設定の実動作への反映には再起動が必要です。':'。');
 }catch(e){setupStatus.textContent='保存失敗: '+e.message;}
}
const setupServerUrl=document.getElementById('setupServerUrl');
const setupServerSave=document.getElementById('setupServerSave');
const setupServerStatus=document.getElementById('setupServerStatus');
setupServerSave.addEventListener('click',async()=>{
 const value=setupServerUrl.value.trim();setupServerUrl.value=value;
 if(!setupServerUrl.reportValidity())return;
 if(!/^https?:\/\/[^\s/?#@]+\/?$/.test(value)){
  setupServerStatus.textContent='http:// または https:// で始まる、パスなしのURLを入力してください。';return;
 }
 setupServerSave.disabled=true;setupServerStatus.textContent='NVSへ保存中…';
 try{
  const response=await fetch('/api/stage1/setup',{method:'POST',
   headers:{'Content-Type':'application/json'},
   body:JSON.stringify({field:'server_base_url',value})});
  if(!response.ok)throw Error('HTTP '+response.status+': '+await response.text());
  const result=await response.json();
  await setupLoad();
  setupServerStatus.textContent='保存しました。SDバックアップは非同期です。本体を再起動すると新しい接続先を使用します。';
  if(!result.reboot_required)setupServerStatus.textContent+=' 再起動要求を確認できませんでした。';
 }catch(e){setupServerStatus.textContent='保存失敗: '+e.message;}
 finally{setupServerSave.disabled=false;}
});
function setupField(holder,field,label,options,value,app){
 const row=document.createElement('div');row.className='setup-row';
 const caption=document.createElement('label');caption.textContent=label;
 const select=document.createElement('select');
 for(const [entry,text] of options){
   const option=document.createElement('option');option.value=String(entry);
   option.textContent=text;select.append(option);
 }
 select.value=String(value);
 if(select.selectedIndex<0){
  const unknown=document.createElement('option');unknown.value=String(value);
  unknown.textContent='現在値: '+value+'（本体の保存値）';
  select.prepend(unknown);select.value=String(value);
 }
 caption.append(select);
 const button=document.createElement('button');button.type='button';button.textContent='保存';
 button.addEventListener('click',async()=>{
  button.disabled=true;
  try{
   const selected=select.value;
   const bool=selected==='true'?true:selected==='false'?false:null;
   const data=bool!==null?bool:
      (/^-?[0-9]+$/.test(selected)?Number(selected):selected);
   await setupSave(field,data,app);
  }finally{button.disabled=false;}
 });
 row.append(caption,button);holder.append(row);
}
function setupInfo(label,value){
 const row=document.createElement('div');row.className='setup-ro';
 const title=document.createElement('b');title.textContent=label;
 const text=document.createElement('span');text.textContent=value??'—';
 row.append(title,text);document.getElementById('setupInfoRows').append(row);
}
async function setupLoad(){
 setupStatus.textContent='本体SETUPの設定を読み込み中…';
 try{
  const response=await fetch('/api/stage1/setup',{cache:'no-store'});
  if(!response.ok)throw Error('HTTP '+response.status+': '+await response.text());
  const d=await response.json();
  const device=document.getElementById('setupDeviceFields');device.replaceChildren();
  const ai=document.getElementById('setupAiFields');ai.replaceChildren();
  const apps=document.getElementById('setupAppFields');apps.replaceChildren();
  document.getElementById('setupInfoRows').replaceChildren();
  setupField(device,'brightness','画面の明るさ',[1,15,30,45,60,75,90,100].map(n=>[n,n+'%']),d.brightness);
  setupField(device,'volume','スピーカー音量',Array.from({length:21},(_,i)=>[i*5,i*5+'%']),d.volume);
  setupField(device,'timezone','タイムゾーン',setupTz,d.timezone);
  setupField(ai,'idle_movement','待機中のランダム動作',[[0,'なし'],[1,'少なめ'],[2,'普通'],[3,'多め']],d.idle_movement);
  setupField(ai,'start_ai_on_boot','起動時にAIを自動開始',[[false,'無効'],[true,'有効']],d.start_ai_on_boot);
  setupServerUrl.value=d.server_base_url||'';
  setupServerSave.disabled=false;
  if(d.app_visibility_loaded){
   for(let i=0;i<setupAppNames.length;i++){
    const [key,label]=setupAppNames[i];
    setupField(apps,'app_visibility',label,[[false,'非表示'],[true,'表示']],Boolean(d.app_visibility_mask&(1<<i)),key);
   }
  }else{apps.textContent='NVSからアプリ表示設定を読み込めません。編集を無効にしました。';}
  setupInfo('アカウント名',d.account_username||'未取得');
  setupInfo('端末名',d.device_name||'未取得');
  setupInfo('ファームウェア',d.firmware_version);
  setupInfo('Wi-Fi MAC',d.mac);
  setupInfo('バッテリー',d.battery+'%'+(d.charging?'（充電中）':''));
  setupStatus.textContent=d.sd_pending?'読み込み完了 / SDバックアップ同期待ち':'読み込み完了';
  return true;
 }catch(e){setupStatus.textContent='読込失敗: '+e.message;return false;}
}
document.getElementById('setupReload').addEventListener('click',setupLoad);
// Avoid concurrent large JSON responses on the memory-constrained server.
Promise.resolve().then(load).then(loadVoice).then(loadQuiet).then(loadEnvironment).then(wifiLoad).then(setupLoad);
</script></body></html>)STAGE1PAGE";

void Reply(httpd_req_t* req, const char* status, const char* body) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, body);
}
// A partial JSON request must not occupy the HTTPD worker indefinitely.
// This deadline covers the complete body rather than restarting per chunk.
bool ReadBody(httpd_req_t* req, char* body, int length) {
    constexpr int64_t kBodyReadDeadlineUs = 10000000;
    const int64_t deadline = esp_timer_get_time() + kBodyReadDeadlineUs;
    int received = 0;
    while (received < length) {
        if (esp_timer_get_time() >= deadline) return false;
        const int count = httpd_req_recv(req, body + received, length - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (count <= 0) return false;
        received += count;
    }
    return true;
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
esp_err_t ReadQuiet(httpd_req_t* req) {
    const auto s = quiet_mode::GetStatus();
    const auto& v = s.settings;
    char json[512];
    std::snprintf(json, sizeof(json),
        "{\"active\":%s,\"clock_ready\":%s,\"until\":%u,\"now\":%u,\"remaining_seconds\":%u,"
        "\"settings\":{\"enabled\":%s,\"daily\":%s,\"start_time\":\"%02u:%02u\",\"end_time\":\"%02u:%02u\","
        "\"once_start\":%u,\"once_end\":%u,\"temporary_until\":%u,\"skip_until\":%u}}",
        s.active?"true":"false", s.clock_ready?"true":"false", (unsigned)s.until, (unsigned)s.now,
        (unsigned)(s.active?s.until-s.now:0), v.enabled?"true":"false", v.daily?"true":"false",
        (unsigned)(v.start_minute/60), (unsigned)(v.start_minute%60), (unsigned)(v.end_minute/60), (unsigned)(v.end_minute%60),
        (unsigned)v.once_start, (unsigned)v.once_end, (unsigned)v.temporary_until, (unsigned)v.skip_until);
    Reply(req, "200 OK", json);
    return ESP_OK;
}
bool JsonEscape(const char* source, char* output, size_t capacity) {
    if (!source || !output || capacity == 0) return false;
    size_t used = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(source); *p; ++p) {
        if (*p == '"' || *p == '\\') {
            if (used + 2 >= capacity) return false;
            output[used++] = '\\'; output[used++] = static_cast<char>(*p);
        } else if (*p < 0x20) {
            if (used + 6 >= capacity) return false;
            const int written = std::snprintf(output + used, capacity - used, "\\u%04x", *p);
            if (written != 6) return false;
            used += 6;
        } else {
            if (used + 1 >= capacity) return false;
            output[used++] = static_cast<char>(*p);
        }
    }
    output[used] = '\0';
    return true;
}
esp_err_t ReadEnvironment(httpd_req_t* req) {
    // Snapshot, escaped strings and the serialized response exceed half of the
    // 4 KiB HTTPD stack. Keep the complete scratch area in PSRAM so it cannot
    // overwrite httpd_req_t while formatting a response.
    struct Scratch {
        environment::Snapshot snapshot;
        environment::Settings settings;
        char region_override[256];
        char region_name[520];
        char city[256];
        char description[256];
        char json[2048];
    };
    Scratch* scratch = static_cast<Scratch*>(
        heap_caps_calloc(1, sizeof(Scratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!scratch) {
        Reply(req, "503 Service Unavailable", "{\"error\":\"PSRAM unavailable\"}");
        return ESP_OK;
    }
    struct ReleaseScratch {
        Scratch* value;
        ~ReleaseScratch() {
            if (!value) return;
            // The saved API key must not remain in a freed PSRAM block.
            std::memset(value, 0, sizeof(*value));
            heap_caps_free(value);
        }
    } release{scratch};
    scratch->snapshot = environment::Read();
    scratch->settings = environment::GetSettings();
    const bool escaped = JsonEscape(scratch->settings.region_name, scratch->region_override,
                                    sizeof(scratch->region_override)) &&
        JsonEscape(scratch->snapshot.region_name, scratch->region_name,
                   sizeof(scratch->region_name)) &&
        JsonEscape(scratch->snapshot.city, scratch->city, sizeof(scratch->city)) &&
        JsonEscape(scratch->snapshot.description, scratch->description,
                   sizeof(scratch->description));
    std::memset(scratch->settings.weather_api_key, 0,
                sizeof(scratch->settings.weather_api_key));
    if (!escaped) {
        Reply(req, "500 Internal Server Error", "{\"error\":\"environment text too long\"}");
        return ESP_OK;
    }
    const int length = std::snprintf(scratch->json, sizeof(scratch->json),
        "{\"initialized\":%s,\"location_valid\":%s,\"weather_valid\":%s,"
        "\"weather_stale\":%s,\"weather_key_set\":%s,\"fetching\":%s,"
        "\"region_override\":\"%s\",\"region_name\":\"%s\",\"city\":\"%s\","
        "\"latitude\":%.6f,\"longitude\":%.6f,\"temperature_c\":%.2f,"
        "\"humidity_percent\":%u,\"condition_code\":%u,\"description\":\"%s\","
        "\"weather_age_minutes\":%llu,\"location_http\":%d,\"weather_http\":%d,"
        "\"sd_pending\":%s}",
        scratch->snapshot.initialized ? "true" : "false",
        scratch->snapshot.location_valid ? "true" : "false",
        scratch->snapshot.weather_valid ? "true" : "false",
        scratch->snapshot.weather_stale ? "true" : "false",
        scratch->snapshot.weather_key_set ? "true" : "false",
        scratch->snapshot.fetching ? "true" : "false",
        scratch->region_override, scratch->region_name, scratch->city,
        scratch->snapshot.latitude, scratch->snapshot.longitude,
        scratch->snapshot.temperature_c, scratch->snapshot.humidity_percent,
        scratch->snapshot.condition_code, scratch->description,
        static_cast<unsigned long long>(scratch->snapshot.weather_age_minutes),
        scratch->snapshot.location_http, scratch->snapshot.weather_http,
        stackchan_sd_nvs_backup_pending() ? "true" : "false");
    if (length < 0 || static_cast<size_t>(length) >= sizeof(scratch->json)) {
        Reply(req, "500 Internal Server Error", "{\"error\":\"environment response too large\"}");
        return ESP_OK;
    }
    Reply(req, "200 OK", scratch->json);
    return ESP_OK;
}
esp_err_t WriteEnvironment(httpd_req_t* req) {
    char content_type[64]{};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        std::strncmp(content_type, "application/json", 16) != 0) {
        Reply(req, "415 Unsupported Media Type", "{\"error\":\"JSON required\"}");
        return ESP_OK;
    }
    if (req->content_len < 1 || req->content_len > 512) {
        Reply(req, "413 Payload Too Large", "{\"error\":\"invalid size\"}");
        return ESP_OK;
    }
    char body[513]{};
    if (!ReadBody(req, body, req->content_len)) {
        Reply(req, "408 Request Timeout", "{\"error\":\"request body timeout\"}");
        return ESP_OK;
    }
    cJSON* root = cJSON_ParseWithLength(body, req->content_len);
    const cJSON* region = root ? cJSON_GetObjectItemCaseSensitive(root, "region_name") : nullptr;
    const cJSON* key = root ? cJSON_GetObjectItemCaseSensitive(root, "weather_api_key") : nullptr;
    const cJSON* clear = root ? cJSON_GetObjectItemCaseSensitive(root, "clear_key") : nullptr;
    auto settings = environment::GetSettings();
    const bool valid = cJSON_IsObject(root) && cJSON_IsString(region) &&
        cJSON_IsString(key) && (!clear || cJSON_IsBool(clear)) &&
        std::strlen(region->valuestring) < sizeof(settings.region_name) &&
        std::strlen(key->valuestring) < sizeof(settings.weather_api_key);
    if (valid) {
        std::snprintf(settings.region_name, sizeof(settings.region_name), "%s", region->valuestring);
        if (cJSON_IsTrue(clear)) settings.weather_api_key[0] = '\0';
        if (key->valuestring[0])
            std::snprintf(settings.weather_api_key, sizeof(settings.weather_api_key), "%s", key->valuestring);
    }
    const bool saved = valid && environment::SaveSettings(settings);
    std::memset(settings.weather_api_key, 0, sizeof(settings.weather_api_key));
    cJSON_Delete(root);
    Reply(req, !valid ? "400 Bad Request" : !saved ? "500 Internal Server Error" : "200 OK",
          !valid ? "{\"error\":\"invalid environment settings\"}" :
          !saved ? "{\"error\":\"NVS save failed\"}" :
          "{\"saved_nvs\":true,\"sd_backup_pending\":true}");
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
            cJSON_AddBoolToObject(json, "monologue_enabled", c->monologue_enabled);
            cJSON_AddStringToObject(json, "monologue_prompt", c->monologue_prompt);
            cJSON_AddNumberToObject(json, "snore_min", c->snore_min);
            cJSON_AddNumberToObject(json, "snore_max", c->snore_max);
            cJSON_AddNumberToObject(json, "talk_min", c->talk_min);
            cJSON_AddNumberToObject(json, "talk_max", c->talk_max);
            cJSON_AddNumberToObject(json, "monologue_min", c->monologue_min);
            cJSON_AddNumberToObject(json, "monologue_max", c->monologue_max);
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
    if (!ReadBody(req, buf, req->content_len)) {
        heap_caps_free(memory); heap_caps_free(buf);
        Reply(req,"408 Request Timeout","{\"error\":\"request body timeout\"}"); return ESP_OK;
    }
    const int got = req->content_len;
    buf[got] = 0;
    cJSON* root = cJSON_Parse(buf);
    heap_caps_free(buf);
    const auto* c = root;
    int snore_lo=0, snore_hi=0, talk_lo=0, talk_hi=0, monologue_lo=0, monologue_hi=0;
    const auto* monologue_enabled = cJSON_IsObject(c) ?
        cJSON_GetObjectItemCaseSensitive(c, "monologue_enabled") : nullptr;
    const bool valid = cJSON_IsObject(c) &&
        VoiceText(c,"morning",draft->morning,sizeof(draft->morning)) &&
        VoiceText(c,"bedtime",draft->bedtime,sizeof(draft->bedtime)) &&
        VoiceText(c,"touch",draft->touch,sizeof(draft->touch)) &&
        VoiceText(c,"wake_word",draft->wake_word,sizeof(draft->wake_word)) &&
        VoiceText(c,"timer",draft->timer,sizeof(draft->timer)) &&
        VoiceText(c,"snore",draft->snore,sizeof(draft->snore)) &&
        VoiceText(c,"sleep_talk_prompt",draft->sleep_talk_prompt,sizeof(draft->sleep_talk_prompt)) &&
        cJSON_IsBool(monologue_enabled) &&
        VoiceText(c,"monologue_prompt",draft->monologue_prompt,sizeof(draft->monologue_prompt)) &&
        Uint(c,"snore_min",1,3600,1,snore_lo) && Uint(c,"snore_max",1,3600,1,snore_hi) &&
        Uint(c,"talk_min",1,3600,1,talk_lo) && Uint(c,"talk_max",1,3600,1,talk_hi) &&
        Uint(c,"monologue_min",1,1440,1,monologue_lo) &&
        Uint(c,"monologue_max",1,1440,1,monologue_hi);
    draft->snore_min=snore_lo; draft->snore_max=snore_hi;
    draft->talk_min=talk_lo; draft->talk_max=talk_hi;
    draft->monologue_enabled=cJSON_IsTrue(monologue_enabled);
    draft->monologue_min=monologue_lo; draft->monologue_max=monologue_hi;
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
bool QuietTime(const cJSON* root, const char* key, uint16_t& minute) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || std::strlen(item->valuestring)!=5) return false;
    const char* s = item->valuestring;
    if(s[2]!=':'||s[0]<'0'||s[0]>'9'||s[1]<'0'||s[1]>'9'||s[3]<'0'||s[3]>'9'||s[4]<'0'||s[4]>'9') return false;
    const unsigned h=(s[0]-'0')*10+s[1]-'0', m=(s[3]-'0')*10+s[4]-'0';
    if(h>=24||m>=60)return false;
    minute=h*60+m;return true;
}
esp_err_t WriteQuiet(httpd_req_t* req) {
    char content_type[64]{};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        std::strncmp(content_type, "application/json", 16) != 0) {
        Reply(req,"415 Unsupported Media Type","{\"error\":\"JSON required\"}");return ESP_OK;
    }
    if(req->content_len<1||req->content_len>256){
        Reply(req,"400 Bad Request","{\"error\":\"invalid quiet request\"}");return ESP_OK;
    }
    char body[257]{};
    int received=0;
    while(received<req->content_len){
        const int n=httpd_req_recv(req,body+received,req->content_len-received);
        if(n<=0)return ESP_FAIL;
        received+=n;
    }
    cJSON* root=cJSON_ParseWithLength(body,received);
    using namespace quiet_mode;
    Request request;
    bool valid=cJSON_IsObject(root);
    const auto* action=cJSON_GetObjectItemCaseSensitive(root,"action");
    if(valid){
        const char* name=cJSON_IsString(action)?action->valuestring:"";
        if(!action||!std::strcmp(name,"minutes")){
            int n=0;valid=Uint(root,"minutes",action?1:0,1440,1,n);
            request.action=n?Action::Minutes:Action::Cancel;request.minutes=n;
        } else if(!std::strcmp(name,"cancel"))request.action=Action::Cancel;
        else if(!std::strcmp(name,"until")){
            request.action=Action::Until;valid=QuietTime(root,"end_time",request.end);
        } else if(!std::strcmp(name,"schedule")){
            request.action=Action::Schedule;
            const auto* enabled=cJSON_GetObjectItemCaseSensitive(root,"enabled");
            const auto* daily=cJSON_GetObjectItemCaseSensitive(root,"daily");
            valid=cJSON_IsBool(enabled)&&cJSON_IsBool(daily)&&QuietTime(root,"start_time",request.start)&&
                QuietTime(root,"end_time",request.end)&&request.start!=request.end;
            request.enabled=cJSON_IsTrue(enabled);request.daily=cJSON_IsTrue(daily);
        } else valid=false;
    }
    cJSON_Delete(root);
    const auto result=valid?Apply(request):ESP_ERR_INVALID_ARG;
    if(result==ESP_OK)return ReadQuiet(req);
    Reply(req,(result==ESP_ERR_INVALID_ARG||result==ESP_ERR_INVALID_STATE)?"400 Bad Request":"500 Internal Server Error",
        result==ESP_ERR_INVALID_STATE?"{\"error\":\"clock unavailable\"}":result==ESP_ERR_INVALID_ARG?"{\"error\":\"invalid quiet settings\"}":"{\"error\":\"NVS save failed\"}");
    return ESP_OK;
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
    if (!ReadBody(req, buf, req->content_len)) {
        Reply(req,"408 Request Timeout","{\"error\":\"request body timeout\"}"); return ESP_OK;
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
    if (!ReadBody(req, body, req->content_len)) {
        Reply(req,"408 Request Timeout","{\"error\":\"request body timeout\"}"); return ESP_OK;
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
    if (!ReadBody(req, body, req->content_len)) {
        heap_caps_free(body);
        Reply(req,"408 Request Timeout","{\"error\":\"request body timeout\"}"); return ESP_OK;
    }
    const int received = req->content_len;
    body[received] = '\0';
    cJSON* root = cJSON_ParseWithLength(body, received);
    const cJSON* item = root ? cJSON_GetObjectItemCaseSensitive(root, "text") : nullptr;
    const cJSON* kind_item = root ? cJSON_GetObjectItemCaseSensitive(root, "kind") : nullptr;
    const char* content = cJSON_IsString(item) ? item->valuestring : nullptr;
    const char* kind = cJSON_IsString(kind_item) ? kind_item->valuestring : "test";
    const size_t len = content ? std::strlen(content) : 0;
    const bool valid_kind = std::strcmp(kind,"test")==0 || std::strcmp(kind,"monologue")==0;
    const bool valid = valid_kind && content && len >= 1 && len <= 512;
    bool accepted = false;
    if (valid) {
        LvglLockGuard lock;
        const auto* config = voice::Current();
        if (config) accepted = voice::Request(*config, kind, content, true);
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
    const size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "deferred start: internal_free=%u largest=%u psram=%u",
             static_cast<unsigned>(free_bytes), static_cast<unsigned>(largest),
             static_cast<unsigned>(psram));
    if (free_bytes < MIN_FREE_INTERNAL || largest < MIN_LARGEST_INTERNAL ||
        psram < MIN_FREE_PSRAM) {
        ESP_LOGW(TAG, "web start deferred: insufficient heap; will retry while idle");
        return;
    }
    if (attempted.exchange(true)) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 4096;
    // IDF 6.1 supports allocating the HTTP server task stack from PSRAM.
    // Keep the HTTPD/lwIP kernel and DMA-only allocations on their default heaps.
    config.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    config.max_uri_handlers = 15;
    config.max_open_sockets = 4;
    const esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "port 80 start failed: %s (internal=%u largest=%u psram=%u)",
                 esp_err_to_name(err),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
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
    httpd_uri_t quiet_read{};
    quiet_read.uri = "/api/stage1/quiet"; quiet_read.method = HTTP_GET; quiet_read.handler = ReadQuiet;
    httpd_uri_t quiet_write{};
    quiet_write.uri = "/api/stage1/quiet"; quiet_write.method = HTTP_POST; quiet_write.handler = WriteQuiet;
    httpd_uri_t environment_read{};
    environment_read.uri = "/api/stage1/environment"; environment_read.method = HTTP_GET;
    environment_read.handler = ReadEnvironment;
    httpd_uri_t environment_write{};
    environment_write.uri = "/api/stage1/environment"; environment_write.method = HTTP_POST;
    environment_write.handler = WriteEnvironment;
    httpd_uri_t wifi_read{};
    wifi_read.uri = "/api/stage1/wifi"; wifi_read.method = HTTP_GET;
    wifi_read.handler = stackchan::wifi::web::ReadSettings;
    httpd_uri_t wifi_write{};
    wifi_write.uri = "/api/stage1/wifi"; wifi_write.method = HTTP_POST;
    wifi_write.handler = stackchan::wifi::web::WriteSettings;
    httpd_uri_t setup_read{};
    setup_read.uri = "/api/stage1/setup"; setup_read.method = HTTP_GET;
    setup_read.handler = setup_web::Read;
    httpd_uri_t setup_write{};
    setup_write.uri = "/api/stage1/setup"; setup_write.method = HTTP_POST;
    setup_write.handler = setup_web::Write;
    if (httpd_register_uri_handler(server, &page) != ESP_OK ||
        httpd_register_uri_handler(server, &read) != ESP_OK ||
        httpd_register_uri_handler(server, &write) != ESP_OK ||
        httpd_register_uri_handler(server, &action) != ESP_OK ||
        httpd_register_uri_handler(server, &voice_read) != ESP_OK ||
        httpd_register_uri_handler(server, &voice_write) != ESP_OK ||
        httpd_register_uri_handler(server, &voice_test) != ESP_OK ||
        httpd_register_uri_handler(server, &quiet_read) != ESP_OK ||
        httpd_register_uri_handler(server, &quiet_write) != ESP_OK ||
        httpd_register_uri_handler(server, &environment_read) != ESP_OK ||
        httpd_register_uri_handler(server, &environment_write) != ESP_OK ||
        httpd_register_uri_handler(server, &wifi_read) != ESP_OK ||
        httpd_register_uri_handler(server, &wifi_write) != ESP_OK ||
        httpd_register_uri_handler(server, &setup_read) != ESP_OK ||
        httpd_register_uri_handler(server, &setup_write) != ESP_OK) {
        ESP_LOGE(TAG, "URI registration failed");
        httpd_stop(server);
        server = nullptr;
        attempted.store(false);
        return;
    }
    ESP_LOGI(TAG, "ready: port=80 internal_free=%u largest=%u psram=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}
}
