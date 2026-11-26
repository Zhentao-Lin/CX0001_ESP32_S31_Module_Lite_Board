/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * 04 USB-Serial (on-board USB Serial/JTAG: flashing, logs and a command line)
 * Key concepts: the USB Serial/JTAG controller is a fixed-function USB device built into the
 *               chip, so one USB-C cable covers flashing, logging and console input without an
 *               external USB-UART bridge. CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG only registers the
 *               console VFS; select() and blocking reads on it additionally require the
 *               USB-Serial-JTAG driver, which is installed in app_main() and then bound to that
 *               same VFS, so a single peripheral is not claimed twice.
 * Behavior: the prompt accepts "help", "led on|off", "info" and "echo <text>"; the on-board
 *           LED follows the commands and the replies come back on the same port.
 * Based on the official examples esp-idf examples/peripherals/usb_serial_jtag
 * (usb_serial_jtag_echo / usb_serial_jtag_vfs), adapted into a line-based console.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_private/esp_clk.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

#define LED_GPIO            CONFIG_LED_GPIO
#define LINE_MAX_LEN        80
#define IDLE_POLL_PERIOD_US 100000  // Console poll period while nothing is received
#define READ_CHUNK_LEN      32      // Bytes pulled from stdin per wake-up

static const char *TAG = "usb_serial";
static char s_line[LINE_MAX_LEN];
static size_t s_line_len;
static bool s_led_on;

static void print_help(void)
{
    printf("Commands:\n"
           "  help          - list commands\n"
           "  led on|off    - turn the on-board LED on or off\n"
           "  info          - chip / memory / uptime summary\n"
           "  echo <text>   - echo the text back\n");
}

/* Chip model as a printable name, so the output matches the Arduino track. */
static const char *chip_model_name(esp_chip_model_t model)
{
    switch (model) {
    case CHIP_ESP32P4:
        return "ESP32-P4";
    case CHIP_ESP32S31:
        return "ESP32-S31";
    case CHIP_ESP32S3:
        return "ESP32-S3";
    case CHIP_ESP32C6:
        return "ESP32-C6";
    default:
        return "unknown";
    }
}

static void print_info(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;

    esp_chip_info(&chip_info);
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        flash_size = 0;
    }

    // Same field set and formatting as the Arduino track, so the two lessons can be compared 1:1
    printf("chip        : %s rev %u\n", chip_model_name(chip_info.model), (unsigned)(chip_info.revision / 100));
    printf("cpu freq    : %d MHz\n", esp_clk_cpu_freq() / 1000000);
    printf("flash size  : %" PRIu32 " MB\n", flash_size / (1024U * 1024U));
    printf("psram size  : %" PRIu32 " MB\n", (uint32_t)(esp_psram_get_size() / (1024U * 1024U)));
    printf("free heap   : %" PRIu32 " bytes\n", (uint32_t)esp_get_free_heap_size());
    printf("uptime      : %" PRIu32 " s\n", (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ));
    printf("led         : %s\n", s_led_on ? "on" : "off");
    printf("usb plugged : %d\n", usb_serial_jtag_is_connected() ? 1 : 0);
}

/* Strip the trailing CR/LF/space so that CRLF and LF terminals behave the same. */
static void trim_line(char *line)
{
    size_t len = strlen(line);

    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ')) {
        line[--len] = '\0';
    }
}

static void handle_line(char *line)
{
    trim_line(line);
    if (line[0] == '\0') {
        return;
    }

    if (strcmp(line, "help") == 0) {
        print_help();
    } else if (strcmp(line, "info") == 0) {
        print_info();
    } else if (strcmp(line, "led on") == 0) {
        ESP_ERROR_CHECK(gpio_set_level(LED_GPIO, 1));
        s_led_on = true;
        printf("OK: LED on\n");
    } else if (strcmp(line, "led off") == 0) {
        ESP_ERROR_CHECK(gpio_set_level(LED_GPIO, 0));
        s_led_on = false;
        printf("OK: LED off\n");
    } else if (strncmp(line, "echo ", 5) == 0) {
        printf("echo: %s\n", line + 5);
    } else {
        printf("ERR: unknown command \"%s\", type 'help'\n", line);
    }
}

void app_main(void)
{
    // The console VFS is registered by ESP-IDF, but select() on it needs the USB-Serial-JTAG
    // driver: without the driver usb_serial_jtag_start_select() returns ESP_ERR_INVALID_STATE,
    // esp_vfs_select() gives up at once (EINTR) instead of waiting for the timeout, and the loop
    // below degrades into a busy-wait that starves IDLE0 and trips the task watchdog.
    // Installing the driver and binding the VFS to it makes select() really block, and read()
    // return as soon as at least one byte is available.
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t driver_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&driver_config));
    }
    usb_serial_jtag_vfs_use_driver();

    ESP_ERROR_CHECK(gpio_reset_pin(LED_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(LED_GPIO, 0));

    ESP_LOGI(TAG, "console is bound to the on-board USB Serial/JTAG port");
    printf("\n=== 04 USB-Serial: on-board USB CDC console ===\n");
    printf("usb plugged : %d\n", usb_serial_jtag_is_connected() ? 1 : 0);
    print_help();
    printf("> ");
    fflush(stdout);

    uint8_t chunk[READ_CHUNK_LEN];

    while (true) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(STDIN_FILENO, &read_set);

        struct timeval timeout = {
            .tv_sec = 0,
            .tv_usec = IDLE_POLL_PERIOD_US,
        };

        // Wait for console input, but only for a short while so the task stays responsive
        int ready = select(STDIN_FILENO + 1, &read_set, NULL, NULL, &timeout);
        if (ready < 0) {
            // A hard error must back off as well: retrying instantly would spin the CPU
            ESP_LOGW(TAG, "select() failed (errno %d)", errno);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (ready == 0 || !FD_ISSET(STDIN_FILENO, &read_set)) {
            continue;
        }

        ssize_t received = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (received <= 0) {
            // Read failed, for example the host detached the port: back off instead of spinning
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Byte-by-byte framing: CR ignored, LF ends a line. The echo below is done here because
        // idf.py monitor (ESP-IDF 6.x) no longer echoes keystrokes locally, unlike a plain UART
        // terminal; echoing from the device keeps the console usable in any terminal tool.
        for (ssize_t i = 0; i < received; i++) {
            char c = (char)chunk[i];
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                printf("\n");
                s_line[s_line_len] = '\0';
                handle_line(s_line);
                s_line_len = 0;
                printf("> ");
                fflush(stdout);
            } else if (c == '\b' || c == 0x7F) {
                // Backspace: drop one character and erase it on the terminal
                if (s_line_len > 0) {
                    s_line_len--;
                    printf("\b \b");
                    fflush(stdout);
                }
            } else if (s_line_len < LINE_MAX_LEN - 1) {
                s_line[s_line_len++] = c;
                putchar(c);
                fflush(stdout);
            } else {
                // Line full: drop the whole buffer to avoid executing a truncated command
                s_line_len = 0;
                printf("\nERR: line too long, input discarded\n> ");
                fflush(stdout);
            }
        }
    }
}
