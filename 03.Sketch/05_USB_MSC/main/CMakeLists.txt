# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Unlicense OR CC0-1.0

idf_component_register(SRCS "usb_msc_main.c"
                       INCLUDE_DIRS ""
                       PRIV_REQUIRES esp_tinyusb fatfs sdmmc esp_driver_sdmmc esp_driver_gpio)
