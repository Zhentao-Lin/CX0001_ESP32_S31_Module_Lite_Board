/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * 01 GPIO output - LED (on-board LED, GPIO38)
 * Key concepts: GPIO output mode, locating the on-board LED pin.
 * Behavior: the on-board LED blinks every 0.3 s.
 * Note: sections 01-03 deliberately use no serial output, because the on-board USB serial port
 *       is only introduced in section 04; every effect here is observed on the LED itself.
 * Based on the official example esp-idf examples/peripherals/gpio (blink),
 * adapted to drive this board's LED pin GPIO38.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "driver/gpio.h"

#define BLINK_GPIO  CONFIG_BLINK_GPIO   // On-board LED pin, default GPIO38 (menuconfig -> Example Configuration)
#define BLINK_DELAY_MS  CONFIG_BLINK_DELAY_MS  // ON/OFF duration, default 300 ms

void app_main(void)
{
    // Configure the pin as output: reset -> direction -> level
    ESP_ERROR_CHECK(gpio_reset_pin(BLINK_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT));

    while (1) {
        ESP_ERROR_CHECK(gpio_set_level(BLINK_GPIO, 1));   // Turn the LED on
        vTaskDelay(BLINK_DELAY_MS / portTICK_PERIOD_MS);

        ESP_ERROR_CHECK(gpio_set_level(BLINK_GPIO, 0));   // Turn the LED off
        vTaskDelay(BLINK_DELAY_MS / portTICK_PERIOD_MS);
    }
}
