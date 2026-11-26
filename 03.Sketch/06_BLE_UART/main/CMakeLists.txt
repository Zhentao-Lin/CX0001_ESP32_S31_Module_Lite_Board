# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Unlicense OR CC0-1.0

idf_component_register(SRCS "ble_uart_main.c"
                       INCLUDE_DIRS ""
                       PRIV_REQUIRES bt nvs_flash driver esp_driver_usb_serial_jtag)
