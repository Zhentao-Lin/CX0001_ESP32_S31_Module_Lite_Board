/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
 * Section 10: Web page controls the on-board WS2812 (native HTTP server)
 * [Key points] esp_http_server serves an HTML control page plus a tiny status API, so an
 *              ordinary browser can drive the WS2812 through the led_strip component
 *              (RMT backend). The old raw-TCP text protocol is gone.
 *              Endpoints:
 *                GET /                          -> HTML control page
 *                GET /ctrl?color=..&brightness=.. -> change colour / brightness, returns status
 *                GET /status                    -> current colour + brightness (status echo)
 *              Colours: "red" / "green" / "blue" / "off". Brightness 0-255.
 *              GPIO / brightness default / HTTP port are set via menuconfig.
 *              Wi-Fi comes from the native ESP32-S31 radio (no esp_wifi_remote / esp_hosted).
 * [What you will see] After boot the log prints "Got IP: x.x.x.x -> open http://x.x.x.x".
 *              Open that URL in a phone/PC browser on the same LAN; tap a colour button or
 *              drag the brightness slider and the WS2812 reacts. The page polls /status so
 *              several devices stay in sync.
 * [Reference example] esp-idf http_server "file_server" for the httpd URI-handler API.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "led_strip.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

#define WS2812_LED_NUM      1
#define WS2812_RMT_RES_HZ   (10 * 1000 * 1000)

#define HTTP_PORT           CONFIG_EXAMPLE_HTTP_PORT
#define PARAM_MAX           16

static const char *TAG = "ws2812_web";
static EventGroupHandle_t s_wifi_events;
static int s_retry_num;
static led_strip_handle_t s_strip;

/* Colour + brightness state, shared between the HTTP handlers and the LED driver. */
typedef enum { COLOR_OFF, COLOR_RED, COLOR_GREEN, COLOR_BLUE } color_state_t;
static color_state_t s_color = COLOR_OFF;
static uint8_t s_brightness = CONFIG_EXAMPLE_WS2812_BRIGHTNESS;

/* --- WS2812 hardware --- */
static led_strip_handle_t ws2812_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_EXAMPLE_WS2812_GPIO,
        .max_leds = WS2812_LED_NUM,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        }
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = WS2812_RMT_RES_HZ,
        .mem_block_symbols = 0,
        .flags = {
            .with_dma = false,
        }
    };

    led_strip_handle_t strip = NULL;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip));
    return strip;
}

static void apply_state(void)
{
    uint8_t r = 0, g = 0, b = 0;

    switch (s_color) {
    case COLOR_RED:   r = s_brightness; break;
    case COLOR_GREEN: g = s_brightness; break;
    case COLOR_BLUE:  b = s_brightness; break;
    case COLOR_OFF:   break;
    }

    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

/* Map a colour name from the query string to the state enum. Returns false if unknown. */
static bool color_from_name(const char *name, color_state_t *out)
{
    if (strcasecmp(name, "red") == 0) {
        *out = COLOR_RED;
    } else if (strcasecmp(name, "green") == 0) {
        *out = COLOR_GREEN;
    } else if (strcasecmp(name, "blue") == 0) {
        *out = COLOR_BLUE;
    } else if (strcasecmp(name, "off") == 0) {
        *out = COLOR_OFF;
    } else {
        return false;
    }
    return true;
}

static const char *color_to_name(color_state_t c)
{
    switch (c) {
    case COLOR_RED:   return "red";
    case COLOR_GREEN: return "green";
    case COLOR_BLUE:  return "blue";
    case COLOR_OFF:   return "off";
    }
    return "off";
}

/* Build the JSON status body, e.g. {"color":"red","brightness":50} */
static void fill_status_json(char *buf, size_t size)
{
    snprintf(buf, size, "{\"color\":\"%s\",\"brightness\":%u}",
             color_to_name(s_color), (unsigned)s_brightness);
}

/* --- HTML control page (UTF-8; served as-is) --- */
static const char INDEX_HTML[] =
    "<!DOCTYPE html><html lang=zh><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>WS2812 控制</title>"
    "<style>"
    "body{font-family:system-ui,Arial,sans-serif;max-width:420px;margin:24px auto;padding:0 16px;"
    "background:#0f1115;color:#e8eaed;}"
    "h1{font-size:20px;font-weight:600;}"
    ".btns{display:grid;grid-template-columns:repeat(2,1fr);gap:12px;margin:20px 0;}"
    "button{padding:20px;font-size:17px;border:0;border-radius:12px;color:#fff;cursor:pointer;"
    "transition:transform .05s,opacity .2s;opacity:.85;}"
    "button:active{transform:scale(.97);}"
    "button.on{opacity:1;box-shadow:0 0 0 3px #fff inset;}"
    ".red{background:#e53935;}.green{background:#43a047;}.blue{background:#1e88e5;}.off{background:#424242;}"
    ".row{display:flex;align-items:center;gap:12px;margin:14px 0;}"
    "input[type=range]{flex:1;}"
    "#state{margin-top:16px;padding:12px;border-radius:10px;background:#1a1d24;font-size:15px;}"
    "#dot{display:inline-block;width:14px;height:14px;border-radius:50%;vertical-align:middle;"
    "margin-right:8px;background:#000;border:1px solid #555;}"
    "</style></head><body>"
    "<h1>板载 WS2812 控制</h1>"
    "<div class=btns>"
    "<button class=red data-c=red onclick=\"send('red')\">红</button>"
    "<button class=green data-c=green onclick=\"send('green')\">绿</button>"
    "<button class=blue data-c=blue onclick=\"send('blue')\">蓝</button>"
    "<button class=off data-c=off onclick=\"send('off')\">灭</button>"
    "</div>"
    "<div class=row><span>亮度</span>"
    "<input id=b type=range min=0 max=255 value=50 oninput=\"dragging=1;bv.textContent=this.value\" onchange=\"dragging=0;send()\">"
    "<span id=bv style=\"min-width:34px;text-align:right\">50</span>"
    "<button style=\"padding:10px 14px\" onclick=\"send()\">应用</button></div>"
    "<div id=state><span id=dot></span><span id=txt>初始化中…</span></div>"
    "<script>"
    "var cur={color:'off',brightness:50},dragging=0;"
    "function rgb(c){return c=='red'?'#e53935':c=='green'?'#43a047':c=='blue'?'#1e88e5':'#000';}"
    "function show(d){"
    "cur=d;"
    "if(!dragging){document.getElementById('b').value=d.brightness;"
    "document.getElementById('bv').textContent=d.brightness;}"
    "document.getElementById('dot').style.background=rgb(d.color);"
    "document.getElementById('txt').textContent='当前：'+d.color+' 亮度 '+d.brightness;"
    "var bs=document.querySelectorAll('.btns button');"
    "for(var i=0;i<bs.length;i++){bs[i].classList.toggle('on',bs[i].dataset.c==d.color);}"
    "}"
    "function apply(r){r.then(function(x){return x.json();}).then(show).catch(function(){});}"
    "function send(color){"
    "var u='/ctrl?brightness='+document.getElementById('b').value;"
    "if(color)u+='&color='+color;"
    "apply(fetch(u,{cache:'no-store'}));"
    "}"
    "setInterval(function(){apply(fetch('/status',{cache:'no-store'}));},2000);"
    "apply(fetch('/status',{cache:'no-store'}));"
    "</script></body></html>";

/* --- HTTP handlers --- */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char buf[64];
    fill_status_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ctrl_get_handler(httpd_req_t *req)
{
    char query[128];
    char val[PARAM_MAX];
    bool changed = false;

    /* Copy the raw query string ("color=..&brightness=..") so we can parse key/value pairs. */
    query[0] = '\0';
    if (httpd_req_get_url_query_len(req) > 0) {
        httpd_req_get_url_query_str(req, query, sizeof(query));
    }

    /* Optional colour parameter */
    if (httpd_query_key_value(query, "color", val, sizeof(val)) == ESP_OK) {
        color_state_t c;
        if (color_from_name(val, &c)) {
            s_color = c;
            changed = true;
        } else {
            ESP_LOGW(TAG, "unknown color param: %s", val);
        }
    }

    /* Optional brightness parameter (0-255) */
    if (httpd_query_key_value(query, "brightness", val, sizeof(val)) == ESP_OK) {
        long b = strtol(val, NULL, 10);
        if (b < 0) {
            b = 0;
        } else if (b > 255) {
            b = 255;
        }
        if ((uint8_t)b != s_brightness) {
            s_brightness = (uint8_t)b;
            changed = true;
        }
    }

    if (changed) {
        apply_state();
    }
    ESP_LOGI(TAG, "ctrl -> color=%s brightness=%u", color_to_name(s_color),
             (unsigned)s_brightness);

    return status_get_handler(req);
}

static httpd_handle_t start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_PORT;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server on port %d", HTTP_PORT);
        return NULL;
    }

    const httpd_uri_t root_uri = {
        .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL
    };
    const httpd_uri_t ctrl_uri = {
        .uri = "/ctrl", .method = HTTP_GET, .handler = ctrl_get_handler, .user_ctx = NULL
    };
    const httpd_uri_t status_uri = {
        .uri = "/status", .method = HTTP_GET, .handler = status_get_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &ctrl_uri);
    httpd_register_uri_handler(server, &status_uri);

    return server;
}

/* --- Wi-Fi --- */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        if (s_retry_num < CONFIG_EXAMPLE_WIFI_MAX_RETRY) {
            s_retry_num++;
            ESP_LOGW(TAG, "Disconnected (reason %d), retry %d/%d", disc->reason,
                     s_retry_num, CONFIG_EXAMPLE_WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR " -> open http://" IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_start(void)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_EXAMPLE_WIFI_SSID,
            sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_EXAMPLE_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));
    if (strlen(CONFIG_EXAMPLE_WIFI_PASSWORD) == 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_strip = ws2812_init();
    apply_state();                  /* start dark */

    if (!wifi_start()) {
        ESP_LOGE(TAG, "Wi-Fi did not connect (SSID? 2.4 GHz? password?)");
        return;
    }

    if (start_web_server()) {
        ESP_LOGI(TAG, "Web server ready on port %d", HTTP_PORT);
    }
}
