# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Unlicense OR CC0-1.0

idf_component_register(SRCS "gpio_button_main.c"
                       INCLUDE_DIRS ""
                       PRIV_REQUIRES driver esp_driver_gpio esp_timer)
