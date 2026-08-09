#include "net_tcp.h"

#include <errno.h>
#include <string.h>

#include "bridge.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "tcp";

#define CLIENT_SINK_BYTES 8192
#define CHUNK 512

/* How long a client's socket write may block before we call it stalled and shed the
 * chunk. Same reasoning as usb_cdc.c: a dead client must cost bytes, never latency on
 * the UART reader. */
#define CLIENT_TX_TIMEOUT_S 5

typedef struct {
    int sock;
    bridge_sink_t *sink;
    bool in_use;
    uint32_t drops;
} tcp_client_t;

static tcp_client_t s_clients[TCP_MAX_CLIENTS];
static SemaphoreHandle_t s_lock;
static uint8_t s_client_count;
static uint32_t s_total_drops;

uint8_t net_tcp_client_count(void)
{
    return s_client_count;
}

uint32_t net_tcp_tx_drops(void)
{
    return s_total_drops;
}

static void client_release(tcp_client_t *c)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (c->in_use) {
        s_total_drops += c->drops + bridge_sink_drops(c->sink);
        bridge_sink_unregister(c->sink);
        c->sink = NULL;
        close(c->sock);
        c->sock = -1;
        c->in_use = false;
        if (s_client_count > 0) {
            s_client_count--;
        }
    }
    xSemaphoreGive(s_lock);
}

/*
 * Router -> this client.
 *
 * SPEC §5.4.1: IAC escaping is a deliberate transparency exception, off by default. It
 * lives here rather than in the fan-out so that enabling it corrupts only the telnet
 * client's stream, never the USB path or the ring-log.
 */
static void client_tx_task(void *arg)
{
    tcp_client_t *c = (tcp_client_t *)arg;
    uint8_t buf[CHUNK];
    uint8_t esc[CHUNK * 2];

    for (;;) {
        const size_t n = bridge_sink_read(c->sink, buf, sizeof(buf), pdMS_TO_TICKS(200));
        if (!c->in_use) {
            break;
        }
        if (n == 0) {
            continue;
        }

        const uint8_t *out = buf;
        size_t out_len = n;

        if (config_get()->telnet_iac_escape) {
            size_t j = 0;
            for (size_t i = 0; i < n; i++) {
                esc[j++] = buf[i];
                if (buf[i] == 0xFF) {
                    esc[j++] = 0xFF; /* IAC IAC == a literal 0xFF */
                }
            }
            out = esc;
            out_len = j;
        }

        size_t off = 0;
        while (off < out_len) {
            const int w = send(c->sock, out + off, out_len - off, 0);
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                /* Timed out (stalled peer) or the connection is gone. */
                c->drops += (uint32_t)(out_len - off);
                goto done;
            }
            off += (size_t)w;
        }
    }

done:
    client_release(c);
    vTaskDelete(NULL);
}

/* This client -> router. Owns the socket's lifetime; the TX task follows it out. */
static void client_rx_task(void *arg)
{
    tcp_client_t *c = (tcp_client_t *)arg;
    uint8_t buf[CHUNK];

    for (;;) {
        const int n = recv(c->sock, buf, sizeof(buf), 0);
        if (n == 0) {
            ESP_LOGI(TAG, "client closed");
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ESP_LOGI(TAG, "client recv: %d", errno);
            break;
        }
        bridge_write(buf, (size_t)n);
    }

    client_release(c);
    vTaskDelete(NULL);
}

static void accept_client(int sock)
{
    /* SPEC §5.4: TCP_NODELAY so a single keystroke leaves immediately rather than waiting
     * for Nagle to accumulate a segment. Interactive latency, criterion §8.10. */
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /* SPEC §5.4: keepalive so a client that vanished without a FIN is eventually reaped
     * and stops holding a slot. */
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    int idle = 10, interval = 5, count = 3;
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));

    struct timeval tv = {.tv_sec = CLIENT_TX_TIMEOUT_S, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    tcp_client_t *c = NULL;
    for (int i = 0; i < TCP_MAX_CLIENTS; i++) {
        if (!s_clients[i].in_use) {
            c = &s_clients[i];
            break;
        }
    }
    if (c == NULL) {
        xSemaphoreGive(s_lock);
        /* SPEC §5.4: reject beyond the limit with a clear message. */
        const char *msg = "router-bridge: too many clients\r\n";
        send(sock, msg, strlen(msg), 0);
        close(sock);
        ESP_LOGW(TAG, "rejected client: %d slots all busy", TCP_MAX_CLIENTS);
        return;
    }

    char name[16];
    snprintf(name, sizeof(name), "tcp%d", (int)(c - s_clients));
    c->sink = bridge_sink_register(name, CLIENT_SINK_BYTES);
    if (c->sink == NULL) {
        xSemaphoreGive(s_lock);
        close(sock);
        return;
    }

    c->sock = sock;
    c->drops = 0;
    c->in_use = true;
    s_client_count++;
    xSemaphoreGive(s_lock);

    if (xTaskCreate(client_rx_task, "tcp_rx", 4096, c, 9, NULL) != pdPASS ||
        xTaskCreate(client_tx_task, "tcp_tx", 4096, c, 9, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for client tasks");
        client_release(c);
        return;
    }
    ESP_LOGI(TAG, "client attached (%u active)", s_client_count);
}

static void listener_task(void *arg)
{
    (void)arg;

    for (;;) {
        const int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_sock < 0) {
            ESP_LOGE(TAG, "socket: %d", errno);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        int one = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(TCP_CONSOLE_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };

        if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
            listen(listen_sock, TCP_MAX_CLIENTS) != 0) {
            ESP_LOGE(TAG, "bind/listen on :%d failed: %d", TCP_CONSOLE_PORT, errno);
            close(listen_sock);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        /* Bound to INADDR_ANY, so this one listener serves both the STA network and the
         * SoftAP fallback without being restarted when the interface changes. */
        ESP_LOGI(TAG, "Path B up: listening on :%d", TCP_CONSOLE_PORT);

        for (;;) {
            struct sockaddr_storage peer;
            socklen_t peer_len = sizeof(peer);
            const int sock = accept(listen_sock, (struct sockaddr *)&peer, &peer_len);
            if (sock < 0) {
                ESP_LOGW(TAG, "accept: %d — restarting listener", errno);
                break;
            }
            accept_client(sock);
        }

        close(listen_sock);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t net_tcp_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < TCP_MAX_CLIENTS; i++) {
        s_clients[i].sock = -1;
    }

    if (xTaskCreate(listener_task, "tcp_listen", 4096, NULL, 8, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
