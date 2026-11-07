/**
 * @file app_net.c
 * 看板 —— WiFi 接入的实现
 */

#include "app_net.h"

#include <stdio.h>
#include <string.h>

#include <esp_check.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/ip_addr.h>

#include "mosaico_shared.h"

static const char *TAG = "app_net";

#define SSID_MAX  33
#define PASS_MAX  65

/* 共享信息里约定的键(保留字段, Vibe Mode 写入) */
#define KEY_SSID "wifi_ssid"
#define KEY_PASS "wifi_password"

static bool     s_connected = false;
static bool     s_started = false;
static char     s_ip[16] = {0};
static char     s_ssid[SSID_MAX] = {0};
static char     s_pass[PASS_MAX] = {0};
static int      s_retry = 0;

static void net_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_ip[0] = 0;
        /* 指数退避重连, 最多退到 30 秒一次, 别把路由器刷爆 */
        int delay_ms = 1000 << (s_retry < 5 ? s_retry : 5);
        if (delay_ms > 30000) {
            delay_ms = 30000;
        }
        s_retry++;
        ESP_LOGW(TAG, "WiFi 断了, %d ms 后重连", delay_ms);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        esp_wifi_connect();
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        s_retry = 0;
        ESP_LOGI(TAG, "已联网, IP=%s", s_ip);
        return;
    }
}

esp_err_t app_net_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    /* 先把网络栈和事件循环建起来: ESP-Iris 的 TCP 通道和后面所有的
     * IP_EVENT 回调都依赖它们, 即使用户还没配网也一样要有。 */
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(loop_err, TAG, "event loop");
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_RETURN_ON_ERROR(ESP_ERR_NO_MEM, TAG, "create STA netif");
    }

    /* 凭据从 Vibe Mode 的共享信息读; 读不到就说明用户还没配网 */
    if (mosaico_shared_get(KEY_SSID, s_ssid, sizeof(s_ssid)) != ESP_OK || s_ssid[0] == 0) {
        ESP_LOGW(TAG, "共享信息里没有 %s, 设备还没配网", KEY_SSID);
        return ESP_ERR_NOT_FOUND;
    }
    /* 开放网络的密码是空串, 读不到也当空 */
    if (mosaico_shared_get(KEY_PASS, s_pass, sizeof(s_pass)) != ESP_OK) {
        s_pass[0] = 0;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &net_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &net_event_handler, NULL, NULL);

    /* IDF v6 的 wifi_sta_config_t 里没有 ssid_len 了, ssid 本身就是
     * 以 '\0' 结尾的字符串(最多 31 个字符)。 */
    wifi_config_t sta_cfg = {0};
    size_t ssid_len = strnlen(s_ssid, sizeof(sta_cfg.sta.ssid) - 1);
    memcpy(sta_cfg.sta.ssid, s_ssid, ssid_len);
    sta_cfg.sta.ssid[ssid_len] = 0;
    if (s_pass[0] != 0) {
        size_t pass_len = strnlen(s_pass, sizeof(sta_cfg.sta.password) - 1);
        memcpy(sta_cfg.sta.password, s_pass, pass_len);
        sta_cfg.sta.password[pass_len] = 0;
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG, "set STA config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    /* IDF v6 里驱动自己会重连, 没有 esp_wifi_set_auto_reconnect 这个 API 了 */

    s_started = true;
    /* 只打 SSID, 密码永远不进日志 */
    ESP_LOGI(TAG, "开始连接 WiFi: %s", s_ssid);
    return ESP_OK;
}

esp_err_t app_net_stop(void)
{
    if (!s_started) {
        return ESP_OK;
    }
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_started = false;
    s_connected = false;
    s_ip[0] = 0;
    return ESP_OK;
}

bool app_net_is_connected(void)
{
    return s_connected;
}

const char *app_net_ip(void)
{
    return s_ip;
}

const char *app_net_ssid(void)
{
    return s_ssid;
}
