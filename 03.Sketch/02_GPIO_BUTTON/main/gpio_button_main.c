/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * 02 GPIO input - button (BOOT key, GPIO61, used as a user button)
 * Key concepts: GPIO input with pull-up, GPIO interrupt, handing events from the ISR to a task.
 * The mechanical key can still bounce for a few milliseconds, so a software debounce window
 * (BUTTON_DEBOUNCE_MS, 5 ms) is applied in the consumer task.
 * Behavior: every press toggles the on-board LED as visual feedback.
 * Note: sections 01-03 deliberately use no serial output, because the on-board USB serial port
 *       is only introduced in section 04; every effect here is observed on the LED itself.
 * Based on the official example esp-idf examples/peripherals/gpio (button part),
 * adapted to this board's BOOT key on GPIO61 and LED on GPIO38.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#define BUTTON_GPIO         CONFIG_BUTTON_GPIO     // BOOT key, default GPIO61 (menuconfig -> Example Configuration)
#define LED_GPIO            CONFIG_LED_GPIO        // On-board LED, default GPIO38
#define BUTTON_DEBOUNCE_MS  5                      // Software debounce window for the key, in milliseconds

static QueueHandle_t s_press_queue;          // Carries press events from the ISR to app_main()
static int s_led_level;                      // LED state, tracked: reading back an output pin is not reliable
static int64_t s_last_press_us;              // Timestamp of the last accepted press, used for debouncing

// ISR: hand the press over to the task as fast as possible. Filtering is done in
// the task, because debouncing needs a delay and an ISR must not log or block
static void IRAM_ATTR button_isr_handler(void *arg)
{
    uint32_t event = 1;
    xQueueSendFromISR(s_press_queue, &event, NULL);
}

void app_main(void)
{
    s_press_queue = xQueueCreate(10, sizeof(uint32_t));

    // LED as output, off by default
    ESP_ERROR_CHECK(gpio_reset_pin(LED_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(LED_GPIO, 0));

    // BOOT key: input with internal pull-up, interrupt on the falling edge (pressed = low)
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_GPIO, button_isr_handler, NULL));

    uint32_t event;
    while (1) {
        // Wait for the next edge, then decide whether it is a real press
        if (xQueueReceive(s_press_queue, &event, portMAX_DELAY) == pdTRUE) {
            // 1st check: bounce edges arrive right after an accepted press, drop them
            if (esp_timer_get_time() - s_last_press_us < BUTTON_DEBOUNCE_MS * 1000) {
                continue;
            }

            // 2nd check: after one debounce window a real press still holds the key low,
            // while a glitch has already released it and reads back high
            vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
            if (gpio_get_level(BUTTON_GPIO) != 0) {
                continue;
            }

            // Press accepted: toggle the LED as visual feedback
            s_last_press_us = esp_timer_get_time();
            s_led_level = !s_led_level;
            ESP_ERROR_CHECK(gpio_set_level(LED_GPIO, s_led_level));
        }
    }
}
