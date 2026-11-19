/*
 * ESP32-S3 网页 HID 键鼠套装  (ESP-IDF 5.5.x + esp_tinyusb 2.x)
 * ---------------------------------------------------------------------------
 * 插到电脑上 = 一个标准 USB 键盘 + 一个 USB 鼠标（复合设备，免驱）；
 * 手机连板子的 WiFi 热点，打开 http://192.168.4.1 ：
 *   「输入」  输入或粘贴文字 -> 被"敲"进电脑；含中文时自动转 base64
 *   「键盘」  75 配列虚拟键盘
 *   「触控板」手机屏幕当触摸板，控制电脑鼠标
 *
 * 硬件：ESP32-S3 DevKitC-1 (N16R8)
 *   左侧 COM 口 (UART0, GPIO43/44) -> idf.py flash / monitor 用，可正常烧录
 *   右侧 USB 口 (GPIO19/20, 原生 OTG) -> 接目标电脑
 *
 * 限制：HID 只能发扫描码，键盘通道仅支持 ASCII（英文/数字/符号）。
 *       中文走 base64，需要电脑端跑一段解码脚本。
 *       发送的是 US 布局扫描码，目标机若用其它键盘布局，符号会错位。
 * ---------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "mdns.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "class/hid/hid_device.h"

static const char *TAG = "kbd";

/* ============================ 可修改配置 ============================ */
#define FW_VERSION       "1.2.3"
#define AP_SSID          "TinyKeyboard"
#define AP_PASS          "wenqi777"        // 至少 8 位
#define MDNS_HOSTNAME    "espkb"
#define DEFAULT_DELAY_MS 1                // 默认极速档（网页可改）
#define MAX_TEXT_LEN     4096             // 单次请求最大文本长度
#define JOB_QUEUE_LEN    8

/* ============================ USB 描述符 ============================
 * 两个 HID 接口，各自独立端点，互不干扰：
 *   ITF 0 / EP 0x81 : boot 键盘（8 字节报告，BIOS 也能用）
 *   ITF 1 / EP 0x82 : 普通 HID 鼠标（report 鼠标，带滚轮）
 * 比"一个接口塞两个 Report ID"更稳，且键盘仍保持 boot protocol。
 * 需要 sdkconfig 里 CONFIG_TINYUSB_HID_COUNT=2
 * ------------------------------------------------------------------ */
#define ITF_KEYBOARD  0
#define ITF_MOUSE     1

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN)

static const uint8_t keyboard_report_descriptor[] = {TUD_HID_REPORT_DESC_KEYBOARD()};
static const uint8_t mouse_report_descriptor[]   = {TUD_HID_REPORT_DESC_MOUSE()};

/* ---------------------------------------------------------------------------
 * 厂商身份
 *   默认 0x303A/0x4004 = 乐鑫 Espressif，电脑上会显示成 ESP32 相关设备。
 *   下面用罗技 K120 办公键盘的 ID（046D:C31C），电脑就会认为它是一把
 *   "Logitech USB Keyboard"。
 *
 *   注意：VID 是 USB-IF 分配给各厂商的编号。自己玩没问题，
 *        如果要做成商品去卖，请不要冒用别家的 VID。
 * ------------------------------------------------------------------------- */
#define USB_VID 0x046D          /* Logitech */
#define USB_PID 0xC31C          /* K120 有线办公键盘 */

static const tusb_desc_device_t hid_device_descriptor = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x4000,     /* K120 报的是 64.00 */
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x00,       /* K120 本身没有序列号 */
    .bNumConfigurations = 0x01,
};

static const char *hid_string_descriptor[] = {
    (char[]){0x09, 0x04},        // 0: 语言 English(US)
    "Logitech",                  // 1: 厂商
    "USB Keyboard",              // 2: 产品名
    "",                          // 3: 序列号（iSerialNumber=0，不用）
    "USB Keyboard",              // 4: 键盘接口名
    "USB Mouse",                 // 5: 鼠标接口名
};

/* 键盘接口保留 boot protocol（BIOS/UEFI 可用）；鼠标用普通 report 协议。
 * 端点间隔 1ms，和主流的 1000Hz 键鼠一致 */
static const uint8_t hid_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 90),
    TUD_HID_DESCRIPTOR(ITF_KEYBOARD, 4, 1, sizeof(keyboard_report_descriptor), 0x81, 8, 1),
    TUD_HID_DESCRIPTOR(ITF_MOUSE,    5, 0, sizeof(mouse_report_descriptor),   0x82, 8, 1),
};

static const uint8_t nokeys[6] = {0, 0, 0, 0, 0, 0};

/* ============================ HID 回调 ============================ */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return (instance == ITF_MOUSE) ? mouse_report_descriptor : keyboard_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

/* 主机通过 SET_REPORT(Output) 下发键盘 LED 状态，bit1 = Caps Lock。
 * HID 发的是 US 扫描码，大小写由主机的 Caps Lock 状态决定：主机开着 Caps 时，
 * 我们敲的小写字母会被翻成大写，整段 Python 脚本就废了。
 * 把状态记下来给 /status，网页在发送文本前会自动敲一次 Caps Lock 关掉它。 */
static volatile uint8_t s_keyboard_leds = 0;

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
    (void)report_id;
    if (instance == ITF_KEYBOARD && report_type == HID_REPORT_TYPE_OUTPUT && bufsize > 0) {
        s_keyboard_leds = buffer[0];
        ESP_LOGI(TAG, "键盘 LED = 0x%02X (CapsLock=%d)",
                 buffer[0], (buffer[0] & KEYBOARD_LED_CAPSLOCK) ? 1 : 0);
    }
}

/* ============================ 任务队列 ============================ */
typedef struct {
    uint8_t kind;         // 0 = 文本, 1 = 单键/组合键
    uint8_t key;          // HID 键码
    uint8_t mod;          // 修饰键掩码
    uint8_t delay_ms;
    char   *text;
    size_t  len;
} job_t;

static QueueHandle_t s_queue;
static volatile bool s_typing;

/* 打字任务和 HTTP 任务可能同时发 HID 报告，用互斥锁保护 TinyUSB 端点 */
static SemaphoreHandle_t s_hid_mutex;

/* ============================ 键码转换 ============================ */
typedef struct {
    char    ch;
    uint8_t key;
    uint8_t shift;
} ascii_entry_t;

static const ascii_entry_t ASCII_MAP[] = {
    {' ',  HID_KEY_SPACE,          0}, {'!',  HID_KEY_1,              1},
    {'"',  HID_KEY_APOSTROPHE,     1}, {'#',  HID_KEY_3,              1},
    {'$',  HID_KEY_4,              1}, {'%',  HID_KEY_5,              1},
    {'&',  HID_KEY_7,              1}, {'\'', HID_KEY_APOSTROPHE,     0},
    {'(',  HID_KEY_9,              1}, {')',  HID_KEY_0,              1},
    {'*',  HID_KEY_8,              1}, {'+',  HID_KEY_EQUAL,          1},
    {',',  HID_KEY_COMMA,          0}, {'-',  HID_KEY_MINUS,          0},
    {'.',  HID_KEY_PERIOD,         0}, {'/',  HID_KEY_SLASH,          0},
    {'0',  HID_KEY_0,              0}, {'1',  HID_KEY_1,              0},
    {'2',  HID_KEY_2,              0}, {'3',  HID_KEY_3,              0},
    {'4',  HID_KEY_4,              0}, {'5',  HID_KEY_5,              0},
    {'6',  HID_KEY_6,              0}, {'7',  HID_KEY_7,              0},
    {'8',  HID_KEY_8,              0}, {'9',  HID_KEY_9,              0},
    {':',  HID_KEY_SEMICOLON,      1}, {';',  HID_KEY_SEMICOLON,      0},
    {'<',  HID_KEY_COMMA,          1}, {'=',  HID_KEY_EQUAL,          0},
    {'>',  HID_KEY_PERIOD,         1}, {'?',  HID_KEY_SLASH,          1},
    {'@',  HID_KEY_2,              1}, {'[',  HID_KEY_BRACKET_LEFT,   0},
    {'\\', HID_KEY_BACKSLASH,      0}, {']',  HID_KEY_BRACKET_RIGHT,  0},
    {'^',  HID_KEY_6,              1}, {'_',  HID_KEY_MINUS,          1},
    {'`',  HID_KEY_GRAVE,          0}, {'{',  HID_KEY_BRACKET_LEFT,   1},
    {'|',  HID_KEY_BACKSLASH,      1}, {'}',  HID_KEY_BRACKET_RIGHT,  1},
    {'~',  HID_KEY_GRAVE,          1},
};

static bool ascii_to_hid(uint8_t c, uint8_t *key, uint8_t *shift)
{
    *shift = 0;
    if (c >= 'a' && c <= 'z') { *key = HID_KEY_A + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { *key = HID_KEY_A + (c - 'A'); *shift = KEYBOARD_MODIFIER_LEFTSHIFT; return true; }
    if (c >= '1' && c <= '9') { *key = HID_KEY_1 + (c - '1'); return true; }
    if (c == '0')             { *key = HID_KEY_0; return true; }

    for (size_t i = 0; i < sizeof(ASCII_MAP) / sizeof(ASCII_MAP[0]); i++) {
        if ((uint8_t)ASCII_MAP[i].ch == c) {
            *key = ASCII_MAP[i].key;
            if (ASCII_MAP[i].shift) *shift = KEYBOARD_MODIFIER_LEFTSHIFT;
            return true;
        }
    }
    return false;
}

typedef struct { const char *name; uint8_t key; } key_name_t;

static const key_name_t KEY_NAMES[] = {
    {"enter", HID_KEY_ENTER},         {"return", HID_KEY_ENTER},
    {"tab", HID_KEY_TAB},             {"esc", HID_KEY_ESCAPE},
    {"escape", HID_KEY_ESCAPE},       {"backspace", HID_KEY_BACKSPACE},
    {"delete", HID_KEY_DELETE},       {"del", HID_KEY_DELETE},
    {"insert", HID_KEY_INSERT},       {"home", HID_KEY_HOME},
    {"end", HID_KEY_END},             {"pageup", HID_KEY_PAGE_UP},
    {"pagedown", HID_KEY_PAGE_DOWN},  {"space", HID_KEY_SPACE},
    {"caps", HID_KEY_CAPS_LOCK},      {"printscreen", HID_KEY_PRINT_SCREEN},
    {"menu", HID_KEY_APPLICATION},    {"up", HID_KEY_ARROW_UP},
    {"down", HID_KEY_ARROW_DOWN},     {"left", HID_KEY_ARROW_LEFT},
    {"right", HID_KEY_ARROW_RIGHT},   {"f1", HID_KEY_F1},
    {"f2", HID_KEY_F2},               {"f3", HID_KEY_F3},
    {"f4", HID_KEY_F4},               {"f5", HID_KEY_F5},
    {"f6", HID_KEY_F6},               {"f7", HID_KEY_F7},
    {"f8", HID_KEY_F8},               {"f9", HID_KEY_F9},
    {"f10", HID_KEY_F10},             {"f11", HID_KEY_F11},
    {"f12", HID_KEY_F12},
};

static uint8_t lookup_key(const char *name, uint8_t *auto_shift)
{
    *auto_shift = 0;
    if (name[0] && name[1] == '\0') {
        uint8_t k, shift;
        if (ascii_to_hid((uint8_t)name[0], &k, &shift)) {
            *auto_shift = shift;      // 大写字母 / !@# 等符号需要自动带 Shift
            return k;
        }
    }
    for (size_t i = 0; i < sizeof(KEY_NAMES) / sizeof(KEY_NAMES[0]); i++) {
        if (strcmp(name, KEY_NAMES[i].name) == 0) return KEY_NAMES[i].key;
    }
    return 0;
}

/* ============================ 报告发送 ============================ */
static bool kb_report(uint8_t modifier, const uint8_t keys[6])
{
    if (!tud_mounted()) return false;

    TickType_t start = xTaskGetTickCount();
    xSemaphoreTake(s_hid_mutex, portMAX_DELAY);
    while (!tud_hid_n_ready(ITF_KEYBOARD)) {
        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(100)) {
            xSemaphoreGive(s_hid_mutex);
            return false;                     // 主机没在取报告，放弃这一帧
        }
        vTaskDelay(1);
    }
    bool ok = tud_hid_n_keyboard_report(ITF_KEYBOARD, 0, modifier, keys);
    xSemaphoreGive(s_hid_mutex);
    return ok;
}

/* 鼠标：buttons bit0=左 bit1=右 bit2=中；dx/dy/wheel 为相对位移 */
static bool mouse_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    if (!tud_mounted()) return false;

    TickType_t start = xTaskGetTickCount();
    xSemaphoreTake(s_hid_mutex, portMAX_DELAY);
    while (!tud_hid_n_ready(ITF_MOUSE)) {
        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(50)) {
            xSemaphoreGive(s_hid_mutex);
            return false;
        }
        vTaskDelay(1);
    }
    bool ok = tud_hid_n_mouse_report(ITF_MOUSE, 0, buttons, dx, dy, wheel, 0);
    xSemaphoreGive(s_hid_mutex);
    return ok;
}

static void tap_key(uint8_t modifier, uint8_t key, uint8_t hold_ms)
{
    uint8_t keys[6] = {key, 0, 0, 0, 0, 0};
    if (!kb_report(modifier, keys)) return;
    if (hold_ms) vTaskDelay(pdMS_TO_TICKS(hold_ms));  // 让主机至少轮询到一次"按下"
    kb_report(0, nokeys);                             // 释放键 + 释放修饰键
}

static bool wait_mounted(uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    while (!tud_mounted()) {
        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(timeout_ms)) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

static void type_text(const char *s, size_t len, uint8_t delay_ms)
{
    /* 极速档 (delay_ms<=1)：按下只保持 1ms；其它档 3ms，更稳妥
     * 前提是 CONFIG_FREERTOS_HZ=1000，否则这些毫秒值会被量化成 10ms */
    const uint8_t hold_ms = (delay_ms <= 1) ? 1 : 3;

    /* 先发一帧全释放，清掉可能残留的 Shift / Ctrl 状态 */
    kb_report(0, nokeys);

    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c == '\r' || c < 0x20) {
            if (c == '\n')      tap_key(0, HID_KEY_ENTER, hold_ms);
            else if (c == '\t') tap_key(0, HID_KEY_TAB, hold_ms);
            else { /* 其它控制字符忽略 */ }
        } else if (c <= 0x7E) {
            uint8_t key, shift;
            if (ascii_to_hid(c, &key, &shift)) tap_key(shift, key, hold_ms);
        }
        /* c > 0x7E：非 ASCII（中文等），跳过 */
        if (delay_ms) vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    kb_report(0, nokeys);
}

static void typing_task(void *arg)
{
    (void)arg;
    for (;;) {
        job_t *job = NULL;
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE || !job) continue;

        s_typing = true;
        if (!wait_mounted(15000)) {
            ESP_LOGW(TAG, "USB 未连接主机，丢弃本次输入");
        } else if (job->kind == 0) {
            ESP_LOGI(TAG, "输入 %u 字节，间隔 %ums", (unsigned)job->len, job->delay_ms);
            type_text(job->text, job->len, job->delay_ms);
        } else {
            tap_key(job->mod, job->key, 3);   // 单键/组合键保持 3ms，稳一些
        }
        s_typing = false;

        free(job->text);
        free(job);
    }
}

/* ============================ HTTP ============================ */
extern const unsigned char index_html_start[] asm("_binary_index_html_start");
extern const unsigned char index_html_end[]   asm("_binary_index_html_end");

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

static int query_int(httpd_req_t *req, const char *key, int def)
{
    char query[64];
    char val[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return def;
    if (httpd_query_key_value(query, key, val, sizeof(val)) != ESP_OK) return def;
    return atoi(val);
}

/* 读完整请求体，超出 cap 的部分丢弃（避免影响 keep-alive） */
static size_t read_body(httpd_req_t *req, char *buf, size_t cap)
{
    char scratch[512];
    size_t total = 0;
    for (;;) {
        int r = httpd_req_recv(req, scratch, sizeof(scratch));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        if (total < cap) {
            size_t n = (size_t)r;
            if (total + n > cap) n = cap - total;
            memcpy(buf + total, scratch, n);
            total += n;
        }
    }
    if (total < cap) buf[total] = '\0';
    else             buf[cap - 1] = '\0';
    return total;
}

static bool enqueue_job(job_t *job)
{
    return xQueueSend(s_queue, &job, 0) == pdTRUE;
}

static esp_err_t type_post(httpd_req_t *req)
{
    /* 多留 1 字节给 read_body 之外的结尾 '\0'，避免刚好写满时越界 */
    char *body = malloc(MAX_TEXT_LEN + 1);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":0,\"err\":\"nomem\"}");
    }
    size_t len = read_body(req, body, MAX_TEXT_LEN);
    body[len] = '\0';

    int d = query_int(req, "d", DEFAULT_DELAY_MS);
    if (d < 1 || d > 80) d = DEFAULT_DELAY_MS;
    int nl = query_int(req, "nl", 0);

    /* 统计：可发送字符 / 被跳过的非 ASCII 字符 */
    unsigned typed = 0, skipped = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)body[i];
        if (c < 0x80) {
            if (c != '\r') typed++;
        } else {
            skipped++;
            while (i + 1 < len && ((uint8_t)body[i + 1] & 0xC0) == 0x80) i++;
        }
    }

    if (len == 0) {
        free(body);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":0,\"err\":\"empty\"}");
    }

    job_t *job = calloc(1, sizeof(job_t));
    if (!job) {
        free(body);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":0,\"err\":\"nomem\"}");
    }
    job->kind = 0;
    job->delay_ms = (uint8_t)d;
    job->text = body;          // 所有权转移给打字任务
    job->len = len;

    if (!enqueue_job(job)) {
        free(job->text);
        free(job);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"ok\":0,\"err\":\"busy\"}");
    }

    if (nl) {
        job_t *j2 = calloc(1, sizeof(job_t));
        if (j2) {
            j2->kind = 1;
            j2->key = HID_KEY_ENTER;
            if (!enqueue_job(j2)) free(j2);
        }
    }

    char resp[80];
    snprintf(resp, sizeof(resp), "{\"ok\":1,\"typed\":%u,\"skipped\":%u}", typed, skipped);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t key_post(httpd_req_t *req)
{
    char name[32] = {0};
    char query[128];
    bool has_query = (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK);

    /* 键名优先从 body 读：符号键 [ ] \ ; ' / ? & + = 等放进 query 会被 URL 规则吃掉。
     * esp-idf 的 httpd_query_key_value() 不做 URL 解码，所以不能直接塞符号。 */
    if (req->content_len > 0) {
        read_body(req, name, sizeof(name));
    }
    if (name[0] == '\0' && has_query) {
        httpd_query_key_value(query, "k", name, sizeof(name));
    }
    if (name[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":0,\"err\":\"nokey\"}");
    }

    /* 只有多字符的名字（enter / f5 ...）才转小写；单字符要保留大小写，A != a */
    if (name[1] != '\0') {
        for (char *p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
    }

    /* 键盘页的 Ctrl / Shift / Alt / Win 单点：发标准的「修饰键按下 + 抬起」
     * 报告（modifier 字节置位、keycode 数组全 0）。KEY_NAMES 里没有这些名字，
     * 之前会被判成 badkey，网页端看就是「点了没反应」。
     * 注意不能把 0xE0~0xE3 塞进 keycode 数组，那种报告部分主机会忽略。 */
    uint8_t mod_only = 0;
    if      (strcmp(name, "ctrl")  == 0) mod_only = KEYBOARD_MODIFIER_LEFTCTRL;
    else if (strcmp(name, "shift") == 0) mod_only = KEYBOARD_MODIFIER_LEFTSHIFT;
    else if (strcmp(name, "alt")   == 0) mod_only = KEYBOARD_MODIFIER_LEFTALT;
    else if (strcmp(name, "gui") == 0 || strcmp(name, "win") == 0) {
        mod_only = KEYBOARD_MODIFIER_LEFTGUI;
    }

    uint8_t auto_shift = 0;
    uint8_t code = lookup_key(name, &auto_shift);
    if (!code && !mod_only) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":0,\"err\":\"badkey\"}");
    }
    if (mod_only) code = 0;

    uint8_t mod = auto_shift | mod_only;
    char mods[32];
    if (has_query && httpd_query_key_value(query, "mod", mods, sizeof(mods)) == ESP_OK) {
        for (char *p = mods; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (strstr(mods, "ctrl"))  mod |= KEYBOARD_MODIFIER_LEFTCTRL;
        if (strstr(mods, "shift")) mod |= KEYBOARD_MODIFIER_LEFTSHIFT;
        if (strstr(mods, "alt"))   mod |= KEYBOARD_MODIFIER_LEFTALT;
        if (strstr(mods, "gui") || strstr(mods, "win") || strstr(mods, "cmd")) {
            mod |= KEYBOARD_MODIFIER_LEFTGUI;
        }
    }

    job_t *job = calloc(1, sizeof(job_t));
    if (!job) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":0,\"err\":\"nomem\"}");
    }
    job->kind = 1;
    job->key = code;
    job->mod = mod;

    if (!enqueue_job(job)) {
        free(job);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"ok\":0,\"err\":\"busy\"}");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":1}");
}

static int8_t clamp_i8(int v)
{
    if (v < -127) return -127;
    if (v >  127) return  127;
    return (int8_t)v;
}

/* POST /mouse?dx=&dy=&wheel=&btn=
 *   btn bit0=左 bit1=右 bit2=中（电平保持，松开要显式发 btn=0）
 *   位移是相对值，范围 -127..127 */
static esp_err_t mouse_post(httpd_req_t *req)
{
    if (req->content_len > 0) {
        char sink[64];
        read_body(req, sink, sizeof(sink));
    }
    int dx    = query_int(req, "dx", 0);
    int dy    = query_int(req, "dy", 0);
    int wheel = query_int(req, "wheel", 0);
    int btn   = query_int(req, "btn", 0);

    bool ok = mouse_report((uint8_t)(btn & 0x07),
                           clamp_i8(dx), clamp_i8(dy), clamp_i8(wheel));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":1}" : "{\"ok\":0,\"err\":\"notready\"}");
}

static esp_err_t status_get(httpd_req_t *req)
{
    wifi_sta_list_t sta;
    int clients = 0;
    if (esp_wifi_ap_get_sta_list(&sta) == ESP_OK) clients = sta.num;

    char resp[192];
    snprintf(resp, sizeof(resp),
             "{\"ok\":1,\"fw\":\"%s\",\"clients\":%d,\"queue\":%u,\"typing\":%d,\"usb\":%d,\"caps\":%d,\"uptime\":%lu}",
             FW_VERSION,
             clients,
             (unsigned)uxQueueMessagesWaiting(s_queue),
             s_typing ? 1 : 0,
             tud_mounted() ? 1 : 0,
             (s_keyboard_leds & KEYBOARD_LED_CAPSLOCK) ? 1 : 0,
             (unsigned long)(esp_log_timestamp() / 1000));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_sendstr(req, "{\"ok\":0,\"err\":\"404\"}");
}

static void start_httpd(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 8;
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    const httpd_uri_t uris[] = {
        {.uri = "/",       .method = HTTP_GET,  .handler = root_get},
        {.uri = "/type",   .method = HTTP_POST, .handler = type_post},
        {.uri = "/key",    .method = HTTP_POST, .handler = key_post},
        {.uri = "/mouse",  .method = HTTP_POST, .handler = mouse_post},
        {.uri = "/status", .method = HTTP_GET,  .handler = status_get},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uris[i]));
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
    ESP_LOGI(TAG, "HTTP 服务已启动");
}

/* ============================ WiFi AP ============================ */
static void start_ap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();          // 默认 IP 192.168.4.1

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(AP_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    strlcpy((char *)ap.ap.password, AP_PASS, sizeof(ap.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "热点 \"%s\" 已开启，密码 %s", AP_SSID, AP_PASS);
    ESP_LOGI(TAG, "浏览器打开 http://192.168.4.1/");
}

/* ============================ USB 初始化 ============================ */
static void start_usb(void)
{
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG();
    cfg.descriptor.device = &hid_device_descriptor;
    cfg.descriptor.full_speed_config = hid_configuration_descriptor;
    cfg.descriptor.string = hid_string_descriptor;
    cfg.descriptor.string_count = sizeof(hid_string_descriptor) / sizeof(hid_string_descriptor[0]);

    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
    ESP_LOGI(TAG, "USB HID 键鼠已初始化 (VID:PID = %04X:%04X)", USB_VID, USB_PID);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_LOGI(TAG, "=== ESP32-S3 Web Keyboard v%s ===", FW_VERSION);

    s_hid_mutex = xSemaphoreCreateMutex();
    configASSERT(s_hid_mutex);
    s_queue = xQueueCreate(JOB_QUEUE_LEN, sizeof(job_t *));
    configASSERT(s_queue);
    xTaskCreate(typing_task, "typing", 4096, NULL, 5, NULL);

    start_usb();
    start_ap();

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(MDNS_HOSTNAME);
        mdns_instance_name_set("ESP32 Keyboard");
        ESP_LOGI(TAG, "mDNS: http://%s.local", MDNS_HOSTNAME);
    }

    start_httpd();
}
