/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
 * Section 06: BLE data transfer (Nordic UART Service, native ESP32-S31 Bluetooth)
 * [Key points] NimBLE GATT server with the Nordic UART Service: one WRITE characteristic from
 *              the phone, one NOTIFY characteristic back. The ESP32-S31 has the BLE radio and
 *              controller on-chip, so the NimBLE host and controller both run on the S31 itself
 *              (no esp_hosted and no on-board ESP32-C6, as the P4 build needed).
 * [What you will see] Connect with nRF Connect, write to RX, and the same bytes arrive back as
 *              a notification on TX while the console logs them in hex and ASCII.
 * [Reference example] examples/bluetooth/nimble/bleprph (GATT table and advertising skeleton),
 *              with the NUS profile substituted and the echo path added.
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "ble_uart";

#define DEVICE_NAME         "S31-BLE-UART"
#define MAX_PAYLOAD_LEN     256
/* Console read tuning (see 04_USB_SERIAL): polling period when idle and bytes per read */
#define CONSOLE_IDLE_POLL_US 100000
#define READ_CHUNK_LEN       32

/* Nordic UART Service UUIDs (128-bit values are stored little-endian) */
static const ble_uuid128_t s_nus_svc_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_nus_tx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_nus_rx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);

static uint16_t s_tx_val_handle;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;

static int nus_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg);
static void ble_advertise(void);
static void ble_send_to_phone(const uint8_t *data, uint16_t len);

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_nus_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* TX: board -> phone, notify only */
                .uuid = &s_nus_tx_uuid.u,
                .access_cb = nus_access_cb,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_val_handle,
            }, {
                /* RX: phone -> board, write with and without response */
                .uuid = &s_nus_rx_uuid.u,
                .access_cb = nus_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            }, {
                0, /* No more characteristics in this service */
            }
        },
    },
    {
        0, /* No more services */
    },
};

/* Log a payload as hex plus printable ASCII */
static void log_payload(const uint8_t *data, uint16_t len)
{
    char hex[MAX_PAYLOAD_LEN * 3 + 1];
    char text[MAX_PAYLOAD_LEN + 1];
    size_t pos = 0;

    for (uint16_t i = 0; i < len; i++) {
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", data[i]);
        text[i] = (data[i] >= 0x20 && data[i] < 0x7F) ? (char)data[i] : '.';
    }
    hex[pos] = '\0';
    text[len] = '\0';
    ESP_LOGI(TAG, "RX (%u bytes): %s| %s", len, hex, text);
}

/* Push a byte buffer to the connected phone on the TX notify characteristic.
 * Called from the console input task; ble_gatts_notify_custom takes the NimBLE host
 * lock internally on the ATT transmit path, so it is safe to call from any task. */
static void ble_send_to_phone(const uint8_t *data, uint16_t len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "No client connected, dropping %u byte(s)", len);
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om == NULL) {
        ESP_LOGE(TAG, "No mbuf for the TX");
        return;
    }

    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "Notify failed, rc=%d (notifications enabled?)", rc);
    }
}

/* Read console stdin and forward each completed line to the phone. The USB-Serial/JTAG
 * console VFS only supports blocking reads / select() once the USB-Serial-JTAG driver is
 * installed and bound to that VFS (done in app_main); reading it without the driver leaves
 * the bytes undrained and idf.py monitor reports "writing to serial is timing out".
 * Line framing is byte by byte (CR ignored, LF ends a line) and every key is echoed from
 * the device, because ESP-IDF 6.x monitor no longer echoes locally. */
static void console_input_task(void *arg)
{
    uint8_t chunk[READ_CHUNK_LEN];
    char line[MAX_PAYLOAD_LEN + 1];
    size_t len = 0;

    ESP_LOGI(TAG, "Console input ready: type a line and press Enter to send to the phone");

    while (true) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(STDIN_FILENO, &read_set);
        struct timeval timeout = {
            .tv_sec = 0,
            .tv_usec = CONSOLE_IDLE_POLL_US,
        };

        int ready = select(STDIN_FILENO + 1, &read_set, NULL, NULL, &timeout);
        if (ready < 0) {
            ESP_LOGW(TAG, "select() failed (errno %d)", errno);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (ready == 0 || !FD_ISSET(STDIN_FILENO, &read_set)) {
            continue;
        }

        ssize_t received = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (received <= 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        for (ssize_t i = 0; i < received; i++) {
            char c = (char)chunk[i];
            if (c == '\r') {
                continue;
            } else if (c == '\n') {
                putchar('\n');
                fflush(stdout);
                line[len] = '\0';
                if (len > 0) {
                    ESP_LOGI(TAG, "Console -> phone (%u bytes): %s", (unsigned)len, line);
                    ble_send_to_phone((const uint8_t *)line, (uint16_t)len);
                }
                len = 0;
            } else if (c == '\b' || c == 0x7F) {
                if (len > 0) {
                    len--;
                    printf("\b \b");
                    fflush(stdout);
                }
            } else if (len < sizeof(line) - 1) {
                line[len++] = c;
                putchar(c);
                fflush(stdout);
            } else {
                len = 0;
                printf("\nERR: line too long, input discarded\n");
                fflush(stdout);
            }
        }
    }
}

/* Called on every read/write of the NUS characteristics */
static int nus_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t payload[MAX_PAYLOAD_LEN];
    uint16_t len = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, payload, sizeof(payload), &len);
    if (rc != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (len == 0) {
        return 0;
    }

    log_payload(payload, len);

    /* Echo the same bytes back on the notify characteristic */
    struct os_mbuf *om = ble_hs_mbuf_from_flat(payload, len);
    if (om == NULL) {
        ESP_LOGE(TAG, "No mbuf for the echo");
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    rc = ble_gatts_notify_custom(conn_handle, s_tx_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "Notify failed, rc=%d (notifications enabled?)", rc);
    }
    return 0;
}

/* GAP events: connection bookkeeping and re-advertising */
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

/* Start connectable advertising. The 31-byte advertisement only carries the name; the 128-bit
 * service UUID is moved to the scan response, otherwise both do not fit. */
static void ble_advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    struct ble_hs_adv_fields rsp_fields = {0};
    struct ble_gap_adv_params adv_params = {0};
    int rc;

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)ble_svc_gap_device_name();
    fields.name_len = strlen((char *)fields.name);
    fields.name_is_complete = 1;
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed, rc=%d", rc);
        return;
    }

    rsp_fields.uuids128 = &s_nus_svc_uuid;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields failed, rc=%d", rc);
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
    nimble_port_run();              /* Returns only when nimble_port_stop() is called */
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

    /* Native ESP32-S31 Bluetooth: nimble_port_init() brings up the on-chip controller, so no
     * esp_hosted slave-link / controller bring-up is needed here. */
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);

    ESP_ERROR_CHECK(ble_gatts_count_cfg(s_gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(s_gatt_svcs));

    nimble_port_freertos_init(ble_host_task);

    /* The console VFS needs the USB-Serial/JTAG driver for blocking reads / select().
     * Install it (once) and bind the VFS to it, then feed the console into the TX
     * characteristic so the operator can type text and push it to the phone. */
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t usj_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usj_config));
    }
    usb_serial_jtag_vfs_use_driver();

    xTaskCreate(console_input_task, "ble_console", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "BLE stack started, write to RX and the same bytes come back on TX");
}
