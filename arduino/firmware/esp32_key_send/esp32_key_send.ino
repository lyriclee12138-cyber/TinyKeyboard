/*
 * ESP32-S3 网页 HID 键盘  v1.0
 * ---------------------------------------------------------------------------
 * 功能
 *   USB 插入电脑后被识别为标准 HID 键盘（免驱）；手机连接板子的 WiFi 热点，
 *   浏览器打开 http://192.168.4.1，输入或粘贴文字 -> 内容被"敲"进电脑。
 *
 * 硬件：ESP32-S3 DevKitC-1 (N16R8, 16MB Flash + 8MB PSRAM)
 *   左侧 COM 口 (CH343/UART)  -> 烧录 + 看日志，可直接上传，不用按 BOOT
 *   右侧 USB 口 (GPIO19/20)   -> 原生 USB，接目标电脑当键盘
 *
 * Arduino IDE 设置：
 *   开发板            ESP32S3 Dev Module
 *   Flash Size        16MB
 *   PSRAM             OPI PSRAM
 *   USB Mode          USB-OTG (TinyUSB)
 *   USB CDC On Boot   Disabled   <-- Serial 走 UART0(COM 口)，USB 口纯键盘
 *   分区              Default 4MB with spiffs
 *
 * 限制：HID 只能发扫描码，仅支持 ASCII（英文/数字/常用符号）。
 *       中文等非 ASCII 字符会被跳过并在网页上提示。
 * ---------------------------------------------------------------------------
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "USB.h"
#include "USBHIDKeyboard.h"

/* ========================= 可修改配置 ========================= */
static const char    *AP_SSID         = "ESP32-Keyboard";
static const char    *AP_PASS         = "12345678";   // 至少 8 位
static const char    *MDNS_HOST       = "espkb";      // http://espkb.local
static const uint8_t  DEFAULT_DELAY_MS = 8;           // 每字符间隔（网页可调）
static const uint16_t MAX_TEXT_LEN     = 4096;        // 单次请求最大长度
static const uint8_t  QUEUE_DEPTH      = 6;           // 待输入任务队列深度
/* ============================================================= */

USBHIDKeyboard Keyboard;
WebServer      server(80);
QueueHandle_t  jobQueue = nullptr;

volatile bool g_typing = false;

struct Job {
  uint8_t  kind;      // 0 = 文本, 1 = 单键/组合键
  uint8_t  key;       // kind==1 时的键码（KEY_* 或 ASCII）
  uint8_t  mods;      // bit0=Ctrl bit1=Shift bit2=Alt bit3=GUI
  uint16_t delayMs;   // 字符间隔
  String   text;      // kind==0 时的文本
};

/* ============================ 网页 ============================ */
static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="zh-CN"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<title>ESP32 键盘</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--line:#e2e5e9;--tx:#171a1f;--dim:#6b7280;--ac:#2563eb}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;background:var(--bg);color:var(--tx);padding:12px 12px calc(14px + env(safe-area-inset-bottom));
 font:16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif}
header{display:flex;justify-content:space-between;align-items:baseline;margin:2px 2px 10px}
h1{font-size:17px;margin:0;font-weight:600;letter-spacing:.2px}
#st{font-size:12px;color:var(--dim)}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:10px;margin-bottom:10px}
textarea{width:100%;min-height:148px;max-height:44vh;border:1px solid var(--line);border-radius:10px;
 padding:11px;font:16px/1.55 inherit;resize:vertical;background:#fbfcfd;color:var(--tx);outline:none}
textarea:focus{border-color:var(--ac)}
.row{display:flex;gap:8px;margin-top:9px}
button{border:0;border-radius:10px;padding:12px 10px;font-size:15px;font-weight:600;
 background:#eceff3;color:#20242a;flex:1;font-family:inherit;transition:transform .06s,filter .12s}
button:active{transform:scale(.97);filter:brightness(.94)}
button:disabled{opacity:.5}
.primary{background:var(--ac);color:#fff}
.ghost{flex:0 0 auto;padding:12px 14px;font-weight:500;color:var(--dim)}
.opt{display:flex;align-items:center;gap:10px;margin-top:11px;font-size:14px;color:#39404a;flex-wrap:wrap}
select{border:1px solid var(--line);background:#fff;border-radius:8px;padding:7px 9px;font-size:14px;font-family:inherit;color:var(--tx)}
.keys{display:grid;grid-template-columns:repeat(4,1fr);gap:7px}
.keys button{padding:13px 4px;font-size:14px;font-weight:500;background:#f0f3f7}
#toast{position:fixed;left:50%;bottom:calc(20px + env(safe-area-inset-bottom));transform:translate(-50%,16px);
 background:#141821;color:#fff;padding:10px 16px;border-radius:999px;font-size:14px;
 opacity:0;pointer-events:none;transition:.18s;z-index:9;max-width:88vw;text-align:center}
#toast.on{opacity:.96;transform:translate(-50%,0)}
</style></head><body>

<header><h1>ESP32 键盘</h1><span id="st">连接中…</span></header>

<div class="card">
  <textarea id="ta" placeholder="在这里输入或粘贴文字，点「发送」后会被敲进电脑…&#10;（仅支持英文、数字和符号）"></textarea>
  <div class="row">
    <button class="primary" id="bs" onclick="send(false)">发送</button>
    <button id="be" onclick="send(true)">发送 + 回车</button>
    <button class="ghost" onclick="clearAll()">清空</button>
  </div>
  <div class="opt">
    <label>速度
      <select id="spd">
        <option value="20">慢（进 BIOS 用）</option>
        <option value="8" selected>中</option>
        <option value="3">快</option>
      </select>
    </label>
  </div>
</div>

<div class="card">
  <div class="keys">
    <button onclick="key('enter')">Enter</button>
    <button onclick="key('tab')">Tab</button>
    <button onclick="key('esc')">Esc</button>
    <button onclick="key('backspace')">⌫</button>
    <button onclick="key('up')">↑</button>
    <button onclick="key('down')">↓</button>
    <button onclick="key('left')">←</button>
    <button onclick="key('right')">→</button>
    <button onclick="key('a','ctrl')">Ctrl+A</button>
    <button onclick="key('c','ctrl')">Ctrl+C</button>
    <button onclick="key('v','ctrl')">Ctrl+V</button>
    <button onclick="key('z','ctrl')">Ctrl+Z</button>
    <button onclick="key('s','ctrl')">Ctrl+S</button>
    <button onclick="key('f5')">F5</button>
    <button onclick="key('tab','alt')">Alt+Tab</button>
    <button onclick="key('delete')">Del</button>
  </div>
</div>

<div id="toast"></div>
<script>
var ta=document.getElementById('ta'),st=document.getElementById('st');
var CHUNK=400;
function sleep(ms){return new Promise(function(r){setTimeout(r,ms)})}
function toast(m){var t=document.getElementById('toast');t.textContent=m;t.classList.add('on');
 clearTimeout(t._h);t._h=setTimeout(function(){t.classList.remove('on')},1800)}
async function post(url,body){
  var r=await fetch(url,{method:'POST',headers:{'Content-Type':'text/plain;charset=UTF-8'},body:body});
  try{return await r.json()}catch(e){return {ok:0}}
}
function clearAll(){ta.value='';ta.focus()}
async function send(nl){
  var v=ta.value; if(!v.replace(/\s/g,'')){toast('没有内容');return}
  var d=document.getElementById('spd').value;
  var bs=document.getElementById('bs'),be=document.getElementById('be');
  bs.disabled=be.disabled=true;
  var off=0,typed=0,skipped=0,part;
  while(off<v.length){
    part=v.slice(off,off+CHUNK);
    var j=await post('/type?d='+d,part);
    if(!j.ok){ st.textContent='设备忙，重试中…'; await sleep(150); continue; }
    off+=CHUNK; typed+=j.typed; skipped+=j.skipped;
    st.textContent='已发送 '+typed+' 字符…';
  }
  if(nl) await post('/key?k=enter','');
  bs.disabled=be.disabled=false;
  toast('完成：'+typed+' 字符'+(skipped?('，跳过 '+skipped+' 个非 ASCII'):''));
  poll();
}
async function key(k,mod){
  var j=await post('/key?k='+k+(mod?'&mod='+mod:''),'');
  toast(j.ok?(mod?mod.toUpperCase()+'+':'')+k.toUpperCase():'发送失败');
}
async function poll(){
  try{
    var r=await fetch('/status'),j=await r.json();
    st.textContent=j.clients+' 台设备 · 队列 '+j.queue+(j.typing?' · 输入中':'');
  }catch(e){ st.textContent='已断开'; }
}
setInterval(poll,3000); poll();
</script>
</body></html>)HTML";

/* ======================= 键名 -> 键码 ======================= */
struct KeyName { const char *n; uint8_t c; };

static const KeyName KEY_TABLE[] = {
  {"enter", KEY_RETURN}, {"return", KEY_RETURN}, {"tab", KEY_TAB},
  {"esc", KEY_ESC}, {"escape", KEY_ESC}, {"backspace", KEY_BACKSPACE},
  {"delete", KEY_DELETE}, {"del", KEY_DELETE}, {"insert", KEY_INSERT},
  {"home", KEY_HOME}, {"end", KEY_END}, {"pageup", KEY_PAGE_UP},
  {"pagedown", KEY_PAGE_DOWN}, {"space", KEY_SPACE}, {"caps", KEY_CAPS_LOCK},
  {"printscreen", KEY_PRINT_SCREEN}, {"menu", KEY_MENU},
  {"up", KEY_UP_ARROW}, {"down", KEY_DOWN_ARROW},
  {"left", KEY_LEFT_ARROW}, {"right", KEY_RIGHT_ARROW},
  {"f1", KEY_F1},   {"f2", KEY_F2},   {"f3", KEY_F3},   {"f4", KEY_F4},
  {"f5", KEY_F5},   {"f6", KEY_F6},   {"f7", KEY_F7},   {"f8", KEY_F8},
  {"f9", KEY_F9},   {"f10", KEY_F10}, {"f11", KEY_F11}, {"f12", KEY_F12},
};

static uint8_t keyFromName(const String &n) {
  if (n.length() == 1) {
    char c = n[0];
    if (c >= 0x20 && c <= 0x7E) return (uint8_t)c;   // 单字符走 ASCII 映射
  }
  for (size_t i = 0; i < sizeof(KEY_TABLE) / sizeof(KEY_TABLE[0]); i++) {
    if (n == KEY_TABLE[i].n) return KEY_TABLE[i].c;
  }
  return 0;
}

/* ======================= 打字任务 ======================= */
static void tapKeyNow(uint8_t key, uint8_t mods) {
  for (int i = 0; i < 4; i++) {
    if (mods & (1 << i)) {
      Keyboard.pressRaw(0xE0 + i);       // 0xE0=Crtl 0xE1=Shift 0xE2=Alt 0xE3=GUI
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
  Keyboard.press(key);
  vTaskDelay(pdMS_TO_TICKS(10));
  Keyboard.release(key);
  Keyboard.releaseAll();
}

static void typeText(const String &s, uint16_t chDelay) {
  const TickType_t hold = pdMS_TO_TICKS(2);
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (c == '\r') continue;
    if (c >= 0x80) continue;                       // 非 ASCII 直接跳过
    if (c == '\n') {
      Keyboard.press(KEY_RETURN); vTaskDelay(hold); Keyboard.release(KEY_RETURN);
    } else if (c == '\t') {
      Keyboard.press(KEY_TAB);    vTaskDelay(hold); Keyboard.release(KEY_TAB);
    } else if (c >= 0x20) {
      Keyboard.press(c);          vTaskDelay(hold); Keyboard.release(c);
    } else {
      continue;                                    // 其它控制字符忽略
    }
    vTaskDelay(pdMS_TO_TICKS(chDelay));
  }
  Keyboard.releaseAll();
}

static void typingTask(void *) {
  for (;;) {
    Job *job = nullptr;
    if (xQueueReceive(jobQueue, &job, portMAX_DELAY) == pdTRUE && job) {
      g_typing = true;
      if (job->kind == 0) {
        typeText(job->text, job->delayMs ? job->delayMs : DEFAULT_DELAY_MS);
      } else {
        tapKeyNow(job->key, job->mods);
      }
      g_typing = false;
      delete job;
    }
  }
}

/* ======================= HTTP 处理 ======================= */
static void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

static void handleType() {
  String body = server.arg("plain");
  if (body.length() == 0) {
    server.send(400, "application/json", "{\"ok\":0,\"err\":\"empty\"}");
    return;
  }
  if (body.length() > MAX_TEXT_LEN) body = body.substring(0, MAX_TEXT_LEN);

  uint16_t delayMs = DEFAULT_DELAY_MS;
  if (server.hasArg("d")) {
    int v = server.arg("d").toInt();
    if (v >= 1 && v <= 80) delayMs = (uint16_t)v;
  }

  // 统计可发送/被跳过的字符数，用于网页提示
  uint16_t typed = 0, skipped = 0;
  for (size_t i = 0; i < body.length(); i++) {
    uint8_t c = (uint8_t)body[i];
    if (c < 0x80) {
      if (c != '\r') typed++;
    } else {
      skipped++;
      while (i + 1 < body.length() && ((uint8_t)body[i + 1] & 0xC0) == 0x80) i++;
    }
  }

  Job *job = new Job();
  if (!job) { server.send(500, "application/json", "{\"ok\":0,\"err\":\"nomem\"}"); return; }
  job->kind = 0;
  job->key = 0;
  job->mods = 0;
  job->delayMs = delayMs;
  job->text = body;

  if (xQueueSend(jobQueue, &job, 0) != pdTRUE) {
    delete job;
    server.send(503, "application/json", "{\"ok\":0,\"err\":\"busy\"}");
    return;
  }

  if (server.hasArg("nl") && server.arg("nl") == "1") {
    Job *j2 = new Job();
    if (j2) {
      j2->kind = 1; j2->key = KEY_RETURN; j2->mods = 0; j2->delayMs = 0;
      if (xQueueSend(jobQueue, &j2, 0) != pdTRUE) delete j2;
    }
  }

  char buf[72];
  snprintf(buf, sizeof(buf), "{\"ok\":1,\"typed\":%u,\"skipped\":%u}",
           (unsigned)typed, (unsigned)skipped);
  server.send(200, "application/json", buf);
}

static void handleKey() {
  String name = server.arg("k");
  name.toLowerCase();
  uint8_t code = keyFromName(name);
  if (!code) {
    server.send(400, "application/json", "{\"ok\":0,\"err\":\"badkey\"}");
    return;
  }
  uint8_t mods = 0;
  if (server.hasArg("mod")) {
    String m = server.arg("mod");
    m.toLowerCase();
    if (m.indexOf("ctrl")  >= 0) mods |= 0x01;
    if (m.indexOf("shift") >= 0) mods |= 0x02;
    if (m.indexOf("alt")   >= 0) mods |= 0x04;
    if (m.indexOf("gui")   >= 0 || m.indexOf("win") >= 0 || m.indexOf("cmd") >= 0) mods |= 0x08;
  }

  Job *job = new Job();
  if (!job) { server.send(500, "application/json", "{\"ok\":0,\"err\":\"nomem\"}"); return; }
  job->kind = 1;
  job->key = code;
  job->mods = mods;
  job->delayMs = 0;

  if (xQueueSend(jobQueue, &job, 0) != pdTRUE) {
    delete job;
    server.send(503, "application/json", "{\"ok\":0,\"err\":\"busy\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":1}");
}

static void handleStatus() {
  char buf[128];
  snprintf(buf, sizeof(buf),
           "{\"clients\":%d,\"queue\":%u,\"typing\":%d,\"uptime\":%lu}",
           WiFi.softAPgetStationNum(),
           (unsigned)uxQueueMessagesWaiting(jobQueue),
           g_typing ? 1 : 0,
           (unsigned long)(millis() / 1000));
  server.send(200, "application/json", buf);
}

static void handleNotFound() {
  server.send(404, "application/json", "{\"ok\":0,\"err\":\"404\"}");
}

/* ======================= 初始化 ======================= */
static void startAccessPoint() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);

  IPAddress ip(192, 168, 4, 1);
  IPAddress mask(255, 255, 255, 0);
  WiFi.softAPConfig(ip, ip, mask);

  bool ok = WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 4);
  Serial.printf("[WiFi] AP \"%s\" %s, IP=%s\n",
                AP_SSID, ok ? "已启动" : "启动失败",
                WiFi.softAPIP().toString().c_str());

  if (MDNS.begin(MDNS_HOST)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[mDNS] http://%s.local\n", MDNS_HOST);
  }
}

void setup() {
  Serial.begin(115200);      // UART0 -> 左侧 COM 口
  delay(300);
  Serial.println();
  Serial.println("=== ESP32-S3 Web Keyboard ===");

  // 先起 HID，再起 USB 栈，最后留出枚举时间
  Keyboard.begin();
  USB.begin();
  delay(1500);
  Serial.println("[USB] HID 键盘已就绪");

  jobQueue = xQueueCreate(QUEUE_DEPTH, sizeof(Job *));
  xTaskCreatePinnedToCore(typingTask, "typing", 4096, nullptr, 2, nullptr, 0);

  startAccessPoint();

  server.on("/",        HTTP_GET,  handleRoot);
  server.on("/type",    HTTP_POST, handleType);
  server.on("/key",     HTTP_POST, handleKey);
  server.on("/status",  HTTP_GET,  handleStatus);
  server.on("/favicon.ico", HTTP_GET, []() { server.send(204); });
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.printf("[HTTP] http://%s/\n", WiFi.softAPIP().toString().c_str());
  Serial.println("[READY] 手机连热点后打开上面的地址");
}

void loop() {
  server.handleClient();
  delay(1);
}
