/**
 * @file dash_sysstat.c
 * 看板数据层 —— 服务器系统状态
 *
 * PVE 主机占用、磁盘、网络负载这些数据只有服务器那边有,
 * 板子得自己问服务器要, 所以得先知道服务器在哪(地址来自运行时配置)。
 */

#include "dash_sysstat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "dash_http.h"

static const char *TAG = "dash_sysstat";

#define DASH_SYSSTAT_BUF 2048

/* 看板服务器地址, 由 dash_data 在拿到运行时配置后设进来 */
static char s_server_base[128] = {0};

void dash_sysstat_set_server_base(const char * base)
{
    if(base == NULL) {
        s_server_base[0] = 0;
        return;
    }
    snprintf(s_server_base, sizeof(s_server_base), "%s", base);
    /* 去掉末尾多余的 '/', 免得拼出 "//device/..." */
    int n = strlen(s_server_base);
    while(n > 0 && s_server_base[n - 1] == '/') {
        s_server_base[--n] = 0;
    }
    ESP_LOGI(TAG, "看板服务器: %s", s_server_base);
}

int dash_fetch_sysstat(dash_sysstat_t * out)
{
    if(out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if(s_server_base[0] == 0) {
        return -1;
    }

    char url[192];
    snprintf(url, sizeof(url), "%s/device/sysstat", s_server_base);

    char * buf = heap_caps_malloc(DASH_SYSSTAT_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(buf == NULL) {
        buf = heap_caps_malloc(DASH_SYSSTAT_BUF, MALLOC_CAP_8BIT);
    }
    if(buf == NULL) {
        ESP_LOGE(TAG, "sysstat 缓冲分配失败");
        return -1;
    }

    /* 顺便测往返延迟: 这就是"板子到 IPTV 服务器"的真实延迟 */
    int64_t t0 = esp_timer_get_time();
    int n = dash_http_get(url, buf, DASH_SYSSTAT_BUF, 6000, 1);
    int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
    if(n <= 0) {
        heap_caps_free(buf);
        return -1;
    }

    cJSON * root = cJSON_Parse(buf);
    heap_caps_free(buf);
    if(root == NULL) {
        ESP_LOGW(TAG, "sysstat JSON 解析失败");
        return -1;
    }

    /* 主机列表 */
    cJSON * hosts = cJSON_GetObjectItem(root, "hosts");
    cJSON * h = NULL;
    cJSON_ArrayForEach(h, hosts) {
        if(out->host_count >= DASH_HOST_MAX) {
            break;
        }
        dash_host_stat_t * hs = &out->hosts[out->host_count];
        cJSON * v = cJSON_GetObjectItem(h, "n");
        if(v != NULL && cJSON_IsString(v)) {
            snprintf(hs->name, sizeof(hs->name), "%s", v->valuestring);
        }
        v = cJSON_GetObjectItem(h, "ip");
        if(v != NULL && cJSON_IsString(v)) {
            snprintf(hs->ip, sizeof(hs->ip), "%s", v->valuestring);
        }
        hs->cpu_pct = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(h, "cpu"));
        hs->mem_pct = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(h, "mem"));
        hs->steal_pct = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(h, "steal"));
        cJSON * ok = cJSON_GetObjectItem(h, "ok");
        hs->online = (ok != NULL && cJSON_IsTrue(ok));
        out->host_count++;
    }

    out->steal_pct    = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "steal"));
    out->load         = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "load"));
    out->mem_pct      = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "mem"));
    out->disk_pct     = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "disk"));
    out->disk_free_gb = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "disk_free"));
    out->rtt_ms       = (int)dt_ms;
    out->valid        = true;

    cJSON_Delete(root);
    return 0;
}