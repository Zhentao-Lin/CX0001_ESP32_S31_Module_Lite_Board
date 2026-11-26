/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
 * Section 09: Wi-Fi data transfer (socket roles over the native ESP32-S31 Wi-Fi radio)
 * [Key points] One menu-selected mode (menuconfig -> Socket example):
 *              - TCP server: listen on a port, one peer at a time.
 *              - TCP client: connect out to a remote IPv4:port, retry until it answers.
 *              - UDP echo:   datagram echo on a bound port; console lines go to every host on
 *                            the LAN through the broadcast address.
 *              All three modes bridge the link with the USB-Serial/JTAG console: every line typed
 *              and terminated by Enter is sent to the peer, and every chunk the peer sends is
 *              logged as one console line. For that, stdin must be a real blocking source, which
 *              needs the USB-Serial/JTAG driver bound to the console VFS (the same setup as
 *              section 08).
 *              The ESP32-S31 has Wi-Fi on-chip (no esp_wifi_remote / esp_hosted and no on-board
 *              C6); the sockets are ordinary IDF lwIP sockets.
 * [What you will see] `idf.py monitor` doubles as the terminal: type a line and press Enter to
 *              push it to the peer, received text shows up in the same window.
 * [Reference example] examples/protocols/sockets/tcp_server and .../udp_server (merged, with the
 *              Wi-Fi station bring-up from section 10) plus the console input task from
 *              section 08.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "sdkconfig.h"

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

/* The mode and its port(s) come from menuconfig -> Socket example, so only the constants the
 * selected mode needs are defined. */
#if CONFIG_EXAMPLE_SOCKET_MODE_TCP_SERVER
#define TCP_PORT            CONFIG_EXAMPLE_TCP_PORT
#endif
#if CONFIG_EXAMPLE_SOCKET_MODE_TCP_CLIENT
#define REMOTE_IP           CONFIG_EXAMPLE_REMOTE_IP
#define REMOTE_PORT         CONFIG_EXAMPLE_REMOTE_PORT
#endif
#ifdef CONFIG_EXAMPLE_SOCKET_MODE_UDP
#define UDP_PORT            CONFIG_EXAMPLE_UDP_PORT
#define BCAST_PORT          CONFIG_EXAMPLE_REMOTE_PORT
#endif

#define TCP_RX_CHUNK        256
#define UDP_RX_CHUNK        512
/* Console read chunk, line length and queue depth */
#define CONSOLE_CHUNK_LEN     32
#define BRIDGE_LINE_MAX       256
#define BRIDGE_LINE_QUEUE     8
/* How long the sender task waits for a console line before going back to poll the queue */
#define TX_QUEUE_POLL_MS      100
/* UDP watches socket and console queue from one task, so the socket wait needs a timeout */
#define UDP_POLL_US           100000

static const char *TAG = "wifi_socket";
static EventGroupHandle_t s_wifi_events;
static int s_retry_num;
/* Console lines waiting to go out; the item is a NUL-terminated BRIDGE_LINE_MAX buffer */
static QueueHandle_t s_tx_queue;
#if CONFIG_EXAMPLE_SOCKET_MODE_TCP_SERVER || CONFIG_EXAMPLE_SOCKET_MODE_TCP_CLIENT
/* Socket the console lines are sent on, -1 while no peer is connected. Set by the link task on
 * this core and read by the sender task on the other one, hence volatile; an aligned int is
 * written atomically, and reading a stale value only costs one failed send. */
static volatile int s_link_sock = -1;
#endif

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Station started, connecting to \"%s\"", CONFIG_EXAMPLE_WIFI_SSID);
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        if (s_retry_num < CONFIG_EXAMPLE_WIFI_MAX_RETRY) {
            s_retry_num++;
            ESP_LOGW(TAG, "Disconnected (reason %d), retry %d/%d", disc->reason,
                     s_retry_num, CONFIG_EXAMPLE_WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "Disconnected (reason %d), giving up after %d retries",
                     disc->reason, CONFIG_EXAMPLE_WIFI_MAX_RETRY);
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Netmask: " IPSTR ", gateway: " IPSTR,
                 IP2STR(&event->ip_info.netmask), IP2STR(&event->ip_info.gw));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

/* Station bring-up, identical in shape to section 10; returns once an IP is available */
static bool wifi_start(void)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_EXAMPLE_WIFI_SSID,
            sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_EXAMPLE_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));
    if (strlen(CONFIG_EXAMPLE_WIFI_PASSWORD) == 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to \"%s\"", CONFIG_EXAMPLE_WIFI_SSID);
        return true;
    }
    ESP_LOGE(TAG, "Failed to connect to \"%s\" (SSID correct? 2.4 GHz? password?)",
             CONFIG_EXAMPLE_WIFI_SSID);
    return false;
}

/* Read the USB-Serial/JTAG console and queue every completed line. The console VFS only yields
 * bytes once the USB-Serial/JTAG driver is installed and bound to it (done in app_main);
 * reading it without the driver leaves the input undrained and idf.py monitor reports "writing
 * to serial is timing out". read() blocks here, so no select() is needed - and in this file
 * select() is lwip_select(), which only knows about sockets, not about stdin. Line framing is
 * byte by byte (CR ignored, LF ends a line) and every key is echoed from the device, because
 * ESP-IDF 6.x monitor no longer echoes locally. */
static void console_input_task(void *arg)
{
    char line[BRIDGE_LINE_MAX];
    char chunk[CONSOLE_CHUNK_LEN];
    size_t len = 0;

    ESP_LOGI(TAG, "Console input ready: type a line and press Enter to send it to the peer");

    while (1) {
        int received = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (received <= 0) {
            ESP_LOGW(TAG, "Console read returned %d (errno %d)", received, errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        for (int i = 0; i < received; i++) {
            char c = chunk[i];
            if (c == '\r') {
                continue;
            } else if (c == '\n') {
                putchar('\n');
                fflush(stdout);
                line[len] = '\0';
                if (len > 0 && xQueueSend(s_tx_queue, line, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "Send queue full, line dropped");
                }
                len = 0;
            } else if (c == '\b' || c == 0x7F) {
                if (len > 0) {
                    len--;
                    printf("\b \b");
                    fflush(stdout);
                }
            } else if (len < sizeof(line) - 1) {
                line[len++] = c;
                putchar(c);
                fflush(stdout);
            } else {
                len = 0;
                printf("\nERR: line too long, input discarded\n");
                fflush(stdout);
            }
        }
    }
}

/* Fold a received chunk into printable text for one log line: CR/LF would break the line without
 * carrying content, so they are dropped; any other non-printable byte becomes '.'. Returns the
 * number of characters written. */
static size_t to_printable(const char *src, int src_len, char *dst, size_t dst_size)
{
    size_t out = 0;

    for (int i = 0; i < src_len && out < dst_size - 1; i++) {
        char c = src[i];
        if (c == '\r' || c == '\n') {
            continue;
        }
        dst[out++] = (c >= 0x20 && c < 0x7F) ? c : '.';
    }
    dst[out] = '\0';
    return out;
}

#if CONFIG_EXAMPLE_SOCKET_MODE_TCP_SERVER || CONFIG_EXAMPLE_SOCKET_MODE_TCP_CLIENT
/* Send one console line to the peer. send() may accept fewer bytes than asked for, so the body
 * is looped until it is fully out. Returns false if the link cannot take the line. */
static bool line_send(int sock, const char *line)
{
    size_t len = strlen(line);
    size_t sent = 0;

    while (sent < len) {
        int rc = send(sock, line + sent, len - sent, 0);
        if (rc < 0) {
            ESP_LOGW(TAG, "send failed: errno %d", errno);
            return false;
        }
        sent += rc;
    }
    /* Enter is consumed by the console, so the peer would otherwise receive text with no line
     * end; the trailing LF keeps it readable in nc or a plain terminal. */
    if (send(sock, "\n", 1, 0) < 0) {
        ESP_LOGW(TAG, "send of the line feed failed: errno %d", errno);
        return false;
    }
    ESP_LOGI(TAG, "Console -> peer (%u bytes): %s", (unsigned)len, line);
    return true;
}

/* Outbound half of the bridge. It is a separate task because recv() blocks in the link task:
 * one task per direction is what keeps the TCP link full duplex. */
static void sock_tx_task(void *arg)
{
    char line[BRIDGE_LINE_MAX];

    while (1) {
        if (xQueueReceive(s_tx_queue, line, pdMS_TO_TICKS(TX_QUEUE_POLL_MS)) != pdTRUE) {
            continue;
        }
        if (s_link_sock < 0) {
            ESP_LOGW(TAG, "Link down, dropping %u console byte(s)", (unsigned)strlen(line));
            continue;
        }
        line_send(s_link_sock, line);
    }
}

/* Inbound half: print the peer's bytes verbatim until it closes the link. Blocking recv, so the
 * sending side must not live in this loop. */
static void tcp_rx_session(int sock)
{
    char rx_buffer[TCP_RX_CHUNK];

    while (1) {
        int len = recv(sock, rx_buffer, sizeof(rx_buffer), 0);
        if (len > 0) {
            char text[TCP_RX_CHUNK];

            to_printable(rx_buffer, len, text, sizeof(text));
            ESP_LOGI(TAG, "TCP RX %d bytes: %s", len, text);
        } else if (len == 0) {
            ESP_LOGI(TAG, "TCP peer closed the connection");
            return;
        } else {
            ESP_LOGW(TAG, "recv failed: errno %d", errno);
            return;
        }
    }
}

/* Hand the socket to the sender task, run the inbound loop, then take it away again so the
 * sender drops console lines while no peer is connected. */
static void tcp_rx_publish(int sock)
{
    s_link_sock = sock;
    tcp_rx_session(sock);
    s_link_sock = -1;
}
#endif /* TCP bridge modes */

#if CONFIG_EXAMPLE_SOCKET_MODE_TCP_SERVER
/* TCP server mode: listen, take one peer at a time, bridge it with the console */
static void tcp_server_task(void *arg)
{
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(TCP_PORT),
    };

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create TCP socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "TCP bind failed: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    if (listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "TCP listen failed: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "TCP server listening on port %d, waiting for a peer", TCP_PORT);

    while (1) {
        struct sockaddr_in peer_addr;
        socklen_t peer_len = sizeof(peer_addr);

        int client = accept(listen_sock, (struct sockaddr *)&peer_addr, &peer_len);
        if (client < 0) {
            ESP_LOGW(TAG, "accept failed: errno %d", errno);
            continue;
        }
        ESP_LOGI(TAG, "TCP peer connected from %s:%d", inet_ntoa(peer_addr.sin_addr),
                 ntohs(peer_addr.sin_port));

        tcp_rx_publish(client);
        close(client);
    }
}
#endif /* TCP server mode */

#if CONFIG_EXAMPLE_SOCKET_MODE_TCP_CLIENT
/* TCP client mode: reach the remote address, then bridge the link with the console. A peer that
 * is not up yet is retried, so the board may boot before the remote server is running. */
static void tcp_client_task(void *arg)
{
    struct sockaddr_in remote_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(REMOTE_PORT),
    };

    if (inet_pton(AF_INET, REMOTE_IP, &remote_addr.sin_addr) != 1) {
        ESP_LOGE(TAG, "Remote IP \"%s\" is not a dotted-quad address (menuconfig -> Socket example)",
                 REMOTE_IP);
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create TCP socket: errno %d", errno);
            vTaskDelete(NULL);
            return;
        }

        ESP_LOGI(TAG, "Connecting to %s:%d", inet_ntoa(remote_addr.sin_addr), REMOTE_PORT);
        if (connect(sock, (struct sockaddr *)&remote_addr, sizeof(remote_addr)) != 0) {
            ESP_LOGW(TAG, "connect failed: errno %d, retrying in 2 s", errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "TCP peer connected");

        tcp_rx_publish(sock);
        close(sock);

        ESP_LOGW(TAG, "Link gone, reconnecting in 2 s");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
#endif /* TCP client mode */

#ifdef CONFIG_EXAMPLE_SOCKET_MODE_UDP
/* UDP mode keeps the datagram echo and puts the console on top of it: every datagram in is
 * logged and sent straight back to its sender (the original behaviour), while every console line
 * leaves as one datagram. UDP has no connection and the board has no list of peers, so the console
 * side goes to the LAN broadcast address on the configured port - every host listening there
 * receives it, which is also how a peer finds the board without being typed in anywhere. One task
 * watches both sides behind a select() timeout - that works here because the fd really is a
 * socket, which is exactly what the TCP modes' stdin could not do. */
static void udp_bridge_task(void *arg)
{
    char rx_buffer[UDP_RX_CHUNK];
    char text[UDP_RX_CHUNK];
    char line[BRIDGE_LINE_MAX];
    char packet[BRIDGE_LINE_MAX + 1];
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(UDP_PORT),
    };
    /* 255.255.255.255 is the limited broadcast address: one frame to the broadcast MAC reaches
     * every host on the LAN, so there is no address list to keep and no ARP per host. */
    struct sockaddr_in bcast_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
        .sin_port = htons(BCAST_PORT),
    };
    struct sockaddr_in peer_addr;
    socklen_t peer_len = sizeof(peer_addr);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create UDP socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    /* Without this the stack refuses to send to a broadcast destination */
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "UDP bind failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "UDP listening on port %d, console lines broadcast to port %d",
             UDP_PORT, BCAST_PORT);

    while (1) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(sock, &read_set);
        struct timeval timeout = {
            .tv_sec = 0,
            .tv_usec = UDP_POLL_US,
        };

        int ready = select(sock + 1, &read_set, NULL, NULL, &timeout);
        if (ready < 0) {
            ESP_LOGW(TAG, "select() on the UDP socket failed: errno %d", errno);
        } else if (ready > 0 && FD_ISSET(sock, &read_set)) {
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0,
                               (struct sockaddr *)&peer_addr, &peer_len);
            if (len > 0) {
                to_printable(rx_buffer, len, text, sizeof(text));
                ESP_LOGI(TAG, "UDP RX %d bytes from %s:%d: %s", len,
                         inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port), text);

                /* The original echo, unchanged: back to the sender, not broadcast */
                sendto(sock, rx_buffer, len, 0, (struct sockaddr *)&peer_addr, peer_len);
            } else if (len < 0) {
                ESP_LOGW(TAG, "recvfrom failed: errno %d", errno);
            }
        }

        while (xQueueReceive(s_tx_queue, line, 0) == pdTRUE) {
            size_t n = strlen(line);

            /* One console line is one datagram, so the LF has to travel inside the payload -
             * a second sendto would arrive as a separate, empty datagram */
            memcpy(packet, line, n);
            packet[n] = '\n';
            if (sendto(sock, packet, n + 1, 0, (struct sockaddr *)&bcast_addr,
                       sizeof(bcast_addr)) < 0) {
                ESP_LOGW(TAG, "broadcast sendto failed: errno %d", errno);
            } else {
                ESP_LOGI(TAG, "Console -> broadcast :%d (%u bytes): %s",
                         BCAST_PORT, (unsigned)n, line);
            }
        }
    }
}
#endif /* UDP mode */

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (!wifi_start()) {
        return;
    }

    /* Every mode reads the console, so the USB-Serial/JTAG driver must be installed and bound to
     * the console VFS before read() on stdin returns the typed bytes (same two steps as
     * section 08). */
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t usj_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usj_config));
    }
    usb_serial_jtag_vfs_use_driver();

    s_tx_queue = xQueueCreate(BRIDGE_LINE_QUEUE, BRIDGE_LINE_MAX);
    configASSERT(s_tx_queue != NULL);

    xTaskCreate(console_input_task, "sock_console", 4096, NULL, 5, NULL);

#ifdef CONFIG_EXAMPLE_SOCKET_MODE_UDP
    /* The UDP task carries the socket and the console side at once, so it gets the larger stack */
    xTaskCreate(udp_bridge_task, "udp_bridge", 6144, NULL, 4, NULL);
    ESP_LOGI(TAG, "UDP echo mode: port %d echoes back, console lines broadcast to port %d",
             CONFIG_EXAMPLE_UDP_PORT, CONFIG_EXAMPLE_REMOTE_PORT);
#else
    xTaskCreate(sock_tx_task, "sock_tx", 4096, NULL, 4, NULL);
#if CONFIG_EXAMPLE_SOCKET_MODE_TCP_SERVER
    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "TCP server mode: console traffic goes to the peer on port %d", TCP_PORT);
#else
    xTaskCreate(tcp_client_task, "tcp_client", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "TCP client mode: console traffic goes to " CONFIG_EXAMPLE_REMOTE_IP
             ":%d", CONFIG_EXAMPLE_REMOTE_PORT);
#endif
#endif /* UDP mode */
}
