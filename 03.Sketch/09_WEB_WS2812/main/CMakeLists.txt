# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Unlicense OR CC0-1.0

# section 10 main component: a built-in HTTP web page controls the on-board WS2812.
# main has implicit access to all components (same principle as section 09), so no PRIV_REQUIRES.
# idf_component.yml pulls led_strip into the build; esp_http_server ships with the SDK and is
# reachable through main's implicit dependencies. Wi-Fi is the S31's native esp_wifi.
idf_component_register(SRCS "web_ws2812_main.c"
                       INCLUDE_DIRS "")
