# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Unlicense OR CC0-1.0

idf_component_register(SRCS "ble_led_main.c"
                       INCLUDE_DIRS ""
                       PRIV_REQUIRES bt nvs_flash esp_driver_gpio)
