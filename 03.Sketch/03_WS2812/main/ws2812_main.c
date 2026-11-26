/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * 03 WS2812 colour LED (on-board WS2812, GPIO39)
 * [Key points] Addressable RGB LED, RMT backend, GRB byte order, per-channel 0-255 scale.
 * [What you will see] Red -> green -> blue -> white -> off, 300 ms per colour,
 *                     brightness 50 (0-255 scale), looping forever.
 * [Reference example] esp-idf examples/peripherals/rmt/led_strip (espressif/led_strip
 *                     component, RMT backend), reduced to one on-board LED.
 * Note: sections 01-03 deliberately use no serial output, because the on-board USB serial port
 *       is only introduced in section 04; every effect here is observed on the RGB LED itself.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "led_strip.h"

#define WS2812_GPIO        CONFIG_WS2812_GPIO        // Data line pin, default GPIO39
#define WS2812_LED_NUM     CONFIG_WS2812_LED_NUM     // LEDs in the chain, default 1
#define WS2812_BRIGHTNESS  CONFIG_WS2812_BRIGHTNESS  // 0-255 scale, default 50
#define STEP_MS            CONFIG_WS2812_STEP_MS     // Colour dwell time, default 300 ms
#define WS2812_RMT_RES_HZ  CONFIG_WS2812_RMT_RES_HZ  // 10 MHz => 1 tick = 0.1 us, WS2812 timing

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_step_t;

// Full-scale colours; brightness is applied on top of them
static const rgb_step_t s_steps[] = {
    {255, 0, 0},        // red
    {0, 255, 0},        // green
    {0, 0, 255},        // blue
    {255, 255, 255},    // white
    {0, 0, 0},          // off
};

static uint32_t apply_brightness(uint8_t channel)
{
    // Same 0-255 scale as Arduino's setBrightness(): 50 keeps about 20% of the full value
    return (uint32_t)channel * WS2812_BRIGHTNESS / 255;
}

static led_strip_handle_t ws2812_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = WS2812_GPIO,                          // Data line pin
        .max_leds = WS2812_LED_NUM,                             // Number of LEDs in the chain
        .led_model = LED_MODEL_WS2812,                          // WS2812 timing
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,  // WS2812 expects GRB
        .flags = {
            .invert_out = false,                                // Physical output, not inverted
        }
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,                         // Default RMT clock source
        .resolution_hz = WS2812_RMT_RES_HZ,                     // 0.1 us per tick
        .mem_block_symbols = 0,                                 // 0: let the driver size the block
        .flags = {
            .with_dma = false,                                  // One LED does not need DMA
        }
    };

    led_strip_handle_t strip = NULL;
    // Configure the RMT channel and create the LED strip object
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip));
    return strip;
}

void app_main(void)
{
    led_strip_handle_t strip = ws2812_init();

    while (1) {
        for (int i = 0; i < sizeof(s_steps) / sizeof(s_steps[0]); i++) {
            // Write the colour into the driver buffer first ...
            for (int led = 0; led < WS2812_LED_NUM; led++) {
                ESP_ERROR_CHECK(led_strip_set_pixel(strip, led,
                                                    apply_brightness(s_steps[i].r),
                                                    apply_brightness(s_steps[i].g),
                                                    apply_brightness(s_steps[i].b)));
            }
            // ... then push the buffer out on the data line
            ESP_ERROR_CHECK(led_strip_refresh(strip));
            vTaskDelay(pdMS_TO_TICKS(STEP_MS));
        }
    }
}
