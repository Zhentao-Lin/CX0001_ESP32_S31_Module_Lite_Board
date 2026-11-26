/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
 * Section 07: BLE controls the on-board LED (custom GATT service, native ESP32-S31 Bluetooth)
 * [Key points] A custom service with one characteristic that is READ + WRITE + NOTIFY:
 *              writing led_on lights GPIO38, writing led_off clears it, a read returns the current
 *              state and a notification reports every change.
 *              The BLE host and controller both run on-chip on the ESP32-S31 (no esp_hosted,
 *              no on-board ESP32-C6, as the dual-chip P4 build used).
 * [What you will see] Write led_on from nRF Connect and the LED comes on; write led_off and it goes dark;
 *              reading the characteristic returns 0x01 or 0x00.
 * [Reference example] examples/bluetooth/nimble/bleprph (GATT and advertising skeleton) plus
 *              this board's LED pin; the service is a simple 16-bit vendor UUID.
 */
#include <assert.h>
#include <string.h>
#include <strings.h>
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "ble_led";

#define DEVICE_NAME         "S31-BLE-LED"
#define LED_GPIO            38      /* On-board LED, active high */

static const ble_uuid16_t s_led_svc_uuid = BLE_UUID16_INIT(0xFFE0);
static const ble_uuid16_t s_led_chr_uuid = BLE_UUID16_INIT(0xFFE1);

static uint16_t s_led_val_handle;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;
static uint8_t s_led_state;         /* 0 = off, 1 = on */

static int led_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg);
static void ble_advertise(void);

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_led_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_led_chr_uuid.u,
                .access_cb = led_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_led_val_handle,
            }, {
                0, /* No more characteristics in this service */
            }
        },
    },
    {
        0, /* No more services */
    },
};

/* Drive the pin, keep the characteristic value in sync and notify the client */
static void apply_led_state(uint8_t state)
{
    s_led_state = state ? 1 : 0;
    gpio_set_level(LED_GPIO, s_led_state);
    ESP_LOGI(TAG, "LED %s", s_led_state ? "ON" : "OFF");

    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&s_led_state, sizeof(s_led_state));
        if (om != NULL) {
            int rc = ble_gatts_notify_custom(s_conn_handle, s_led_val_handle, om);
            if (rc != 0) {
                ESP_LOGW(TAG, "Notify failed, rc=%d (notifications enabled?)", rc);
            }
        }
    }
}

/* "led_on" turns the LED on, "led_off" turns it off; anything else is not a command */
static int led_parse_command(const uint8_t *data, uint16_t len)
{
    if (len == 6 && strncasecmp((const char *)data, "led_on", 6) == 0) {
        return 1;
    }
    if (len == 7 && strncasecmp((const char *)data, "led_off", 7) == 0) {
        return 0;
    }
    return -1;
}

static int led_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int rc;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        rc = os_mbuf_append(ctxt->om, &s_led_state, sizeof(s_led_state));
        return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;

    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint8_t payload[16];
        uint16_t len = 0;

        rc = ble_hs_mbuf_to_flat(ctxt->om, payload, sizeof(payload), &len);
        if (rc != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        ESP_LOGI(TAG, "Write (%u bytes): \"%.*s\"", len, len, (const char *)payload);

        int cmd = led_parse_command(payload, len);
        if (cmd < 0) {
            ESP_LOGW(TAG, "Unknown command, expected \"led_on\" or \"led_off\"");
            return 0;
        }
        apply_led_state(cmd);
        return 0;
    }

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "Client connected, conn_handle=%d", s_conn_handle);
        } else {
            ESP_LOGW(TAG, "Connection failed, status=%d", event->connect.status);
            ble_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Client disconnected, reason=0x%x", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "Subscribe: attr_handle=%d notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble_advertise();
        return 0;

    default:
        return 0;
    }
}

/* Advertise the name; the 16-bit service UUID also fits in the advertisement */
static void ble_advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    struct ble_gap_adv_params adv_params = {0};
    int rc;

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)ble_svc_gap_device_name();
    fields.name_len = strlen((char *)fields.name);
    fields.name_is_complete = 1;
    fields.uuids16 = &s_led_svc_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed, rc=%d", rc);
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params,
                           gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed, rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "Advertising as \"%s\"", DEVICE_NAME);
}

static void ble_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed, rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed, rc=%d", rc);
        return;
    }
    ble_advertise();
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "Host reset, reason=%d", reason);
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* LED first, so the pin is driven before the first BLE command arrives */
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_set_level(LED_GPIO, 0);

    /* Native ESP32-S31 Bluetooth: nimble_port_init() brings up the on-chip controller, so no
     * esp_hosted slave-link / controller bring-up is needed before starting the NimBLE host. */
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);

    ESP_ERROR_CHECK(ble_gatts_count_cfg(s_gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(s_gatt_svcs));

    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE stack started, write led_on to light the LED, led_off to clear it");
}
