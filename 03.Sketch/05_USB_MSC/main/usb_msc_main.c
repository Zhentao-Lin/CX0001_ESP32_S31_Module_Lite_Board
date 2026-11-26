/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
 * Section 05: virtual USB drive (USB-OTG, an on-board Flash partition becomes a PC drive)
 * [Key points] TinyUSB MSC device with esp_tinyusb: a wear-levelled SPI Flash data partition
 *              is exposed to the host as a removable drive; the file system is owned by the
 *              host (the PC formats it), the application only supplies raw sectors through the
 *              esp_tinyusb MSC storage layer (tinyusb_msc_new_storage_spiflash).
 *              Flash is a fixed medium: there is no card slot and no re-scan, so the old
 *              BOOT-key hot-swap logic (TF card / Q5 power / SDMMC) is gone.
 * [What you will see] Plug the USB-OTG port into a PC: a drive letter appears, format it once,
 *              then copy files in and out; the data survives a power cycle.
 * [Reference example] examples/peripherals/usb/device/tusb_msc (SPI Flash variant, no console
 *              REPL, this board's descriptors)
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

/* How long to wait for the host to finish its pending writes before dropping the medium */
#define STORAGE_DROP_RETRY      5
#define STORAGE_DROP_WAIT_MS    200

/* USB descriptors: one MSC interface, no other function */
#define EPNUM_MSC           1
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL,
};

enum {
    EDPT_MSC_OUT = 0x01,
    EDPT_MSC_IN  = 0x81,
};

static const char *TAG = "usb_msc";
static tinyusb_msc_storage_handle_t s_storage_hdl = NULL;

static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(s_device_desc),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,             /* Espressif VID; change it for a product */
    .idProduct = 0x4002,
    .bcdDevice = 0x100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static uint8_t const s_msc_fs_config[] = {
    /* Config number, interface count, string index, total length, attributes, power (mA) */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, 0, 100),
    /* Interface number, string index, EP out, EP in, EP size */
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};

#if (TUD_OPT_HIGH_SPEED)
static const tusb_desc_device_qualifier_t s_device_qualifier = {
    .bLength = sizeof(s_device_qualifier),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0,
};

static uint8_t const s_msc_hs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, 0, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 512),
};
#endif /* TUD_OPT_HIGH_SPEED */

static char const *s_string_desc[] = {
    (const char[]) { 0x09, 0x04 },   /* 0: language id, English (0x0409) */
    "Espressif",                     /* 1: manufacturer */
    "S31 Flash MSC",                 /* 2: product */
    "123456",                        /* 3: serial number */
    "S31 Flash Storage",             /* 4: MSC interface string */
};

/*
 * Bind the MSC medium to a wear-levelled SPI Flash data partition. The partition table has a
 * FAT-subtype data region (the "fatfs" partition); esp_tinyusb exposes it to the host as raw
 * sectors, so the PC formats and owns the file system and the application never mounts it.
 */
static esp_err_t storage_init_spiflash(wl_handle_t *wl_handle)
{
    ESP_LOGI(TAG, "Initialising wear levelling over the Flash data partition");

    const esp_partition_t *data_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
    if (data_partition == NULL) {
        ESP_LOGE(TAG, "No FAT data partition found: check partitions_16M.csv");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Backing store: '%s' @ 0x%" PRIX32 " (%" PRIu32 " bytes)",
             data_partition->label, data_partition->address, data_partition->size);

    return wl_mount(data_partition, wl_handle);
}

/* Drop the LUN, retrying while the host still has deferred writes pending */
static void storage_detach(void)
{
    if (s_storage_hdl == NULL) {
        return;
    }
    for (int i = 0; i < STORAGE_DROP_RETRY; i++) {
        if (tinyusb_msc_delete_storage(s_storage_hdl) == ESP_OK) {
            s_storage_hdl = NULL;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(STORAGE_DROP_WAIT_MS));
    }
    ESP_LOGW(TAG, "Storage still has pending writes; the flash may be inconsistent");
    s_storage_hdl = NULL;
}

/* Mount point changes are reported here: the host is the owner while it is mounted */
static void storage_event_cb(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event,
                             void *arg)
{
    switch (event->id) {
    case TINYUSB_MSC_EVENT_MOUNT_START:
        ESP_LOGI(TAG, "Host starts mounting the drive");
        break;
    case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
        ESP_LOGI(TAG, "Storage mounted to %s",
                 (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) ? "the application"
                                                                       : "the USB host");
        break;
    case TINYUSB_MSC_EVENT_MOUNT_FAILED:
        ESP_LOGE(TAG, "Mount failed: no usable file system yet (format it on a PC)");
        break;
    case TINYUSB_MSC_EVENT_FORMAT_REQUIRED:
        ESP_LOGE(TAG, "The drive needs a format: do it on a PC, this example never formats");
        break;
    case TINYUSB_MSC_EVENT_FORMAT_FAILED:
        ESP_LOGE(TAG, "Format failed");
        break;
    default:
        break;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Initialising the Flash-backed USB drive");

    /*
     * Install the MSC driver before creating any storage, otherwise the first
     * tinyusb_msc_new_storage_* call installs it with default flags.
     * auto_mount_off keeps the flash on the USB side for ever: by default the component
     * hands the medium to the application on eject and on SET_CONFIGURATION(0), and a LUN
     * owned by the application answers 'medium not present' to the host, which Windows
     * shows as a greyed-out drive asking to insert a disk.
     */
    tinyusb_msc_driver_config_t drv_cfg = {
        .user_flags.auto_mount_off = 1,
        .callback = storage_event_cb,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(tinyusb_msc_install_driver(&drv_cfg));

    tinyusb_msc_storage_config_t storage_cfg = {
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        .fat_fs = {
            .base_path = NULL,
            .config.max_files = 5,
            /* Double protection: with auto_mount_off the flash is never mounted by the app,
             * but if it ever is, a volume that is not FAT (exFAT / NTFS) must not be wiped */
            .do_not_format = true,
            .format_flags = 0,
        },
    };

    static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
    ESP_ERROR_CHECK(storage_init_spiflash(&s_wl_handle));
    storage_cfg.medium.wl_handle = s_wl_handle;

    /* The flash is handed to the USB host straight away; the application does not mount it */
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_spiflash(&storage_cfg, &s_storage_hdl));

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &s_device_desc;
    tusb_cfg.descriptor.full_speed_config = s_msc_fs_config;
    tusb_cfg.descriptor.string = s_string_desc;
    tusb_cfg.descriptor.string_count = sizeof(s_string_desc) / sizeof(s_string_desc[0]);
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = s_msc_hs_config;
    tusb_cfg.descriptor.qualifier = &s_device_qualifier;
#endif

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB MSC ready: connect the USB-OTG port to a PC and format the drive");

    /* The application must not touch the flash while the host owns it, so only report the
     * state when it actually changes; all data movement happens inside the MSC callbacks */
    tinyusb_msc_mount_point_t last_mp = TINYUSB_MSC_STORAGE_MOUNT_USB;
    uint32_t last_count = 0;
    bool first_report = true;

    while (1) {
        uint32_t sector_count = 0;
        tinyusb_msc_mount_point_t mp = last_mp;
        if (s_storage_hdl != NULL &&
            tinyusb_msc_get_storage_mount_point(s_storage_hdl, &mp) == ESP_OK &&
            tinyusb_msc_get_storage_capacity(s_storage_hdl, &sector_count) == ESP_OK) {
            if (first_report || mp != last_mp || sector_count != last_count) {
                ESP_LOGI(TAG, "%s | %" PRIu32 " blocks",
                         (mp == TINYUSB_MSC_STORAGE_MOUNT_USB) ? "owned by the USB host"
                                                               : "owned by the application",
                         sector_count);
                last_mp = mp;
                last_count = sector_count;
                first_report = false;
            }
        } else if (s_storage_hdl == NULL && !first_report) {
            ESP_LOGW(TAG, "No medium available");
            first_report = true;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
