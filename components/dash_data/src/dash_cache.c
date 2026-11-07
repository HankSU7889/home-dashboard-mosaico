/**
 * @file dash_cache.c
 * 看板数据层 —— NVS 缓存(断网时拿上次的数据顶上)
 *
 * 缓存和运行时配置分开放: 配置在 dashcfg, 缓存在 dashcache,
 * 恢复出厂 / 单独清缓存时互不影响。
 */

#include "dash_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <nvs_flash.h>

static const char * TAG = "dash_cache";

#define DASH_CACHE_NS    "dashcache"
#define DASH_CACHE_MAGIC 0x43414348u  /* "CACH" */
#define DASH_CACHE_VER   1

/* 存盘的文件头: magic + 版本 + 数据长度 + 存的时间 */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    int64_t  saved_at;
} dash_cache_hdr_t;

/* 写一个 blob: 文件头 + 数据 */
static void dash_cache_write(const char * key, const void * data, size_t size)
{
    if(data == NULL || size == 0) return;

    size_t total = sizeof(dash_cache_hdr_t) + size;
    uint8_t * buf = (uint8_t *)malloc(total);
    if(buf == NULL) {
        ESP_LOGE(TAG, "缓存缓冲分配失败(%s)", key);
        return;
    }

    dash_cache_hdr_t hdr = {
        .magic    = DASH_CACHE_MAGIC,
        .version  = DASH_CACHE_VER,
        .size     = (uint16_t)size,
        .saved_at = (int64_t)(esp_timer_get_time() / 1000),
    };
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), data, size);

    nvs_handle_t h;
    if(nvs_open(DASH_CACHE_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败(%s)", key);
        free(buf);
        return;
    }
    esp_err_t err = nvs_set_blob(h, key, buf, total);
    if(err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    free(buf);

    if(err != ESP_OK) {
        ESP_LOGE(TAG, "缓存写入失败 %s: %s", key, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "缓存已存 %s (%d 字节)", key, (int)size);
    }
}

/* 读一个 blob: 校验文件头, 把数据拷出来 */
static bool dash_cache_read(const char * key, void * out, size_t expect_size)
{
    nvs_handle_t h;
    if(nvs_open(DASH_CACHE_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    size_t len = 0;
    if(nvs_get_blob(h, key, NULL, &len) != ESP_OK || len < sizeof(dash_cache_hdr_t)) {
        nvs_close(h);
        return false;
    }

    uint8_t * buf = (uint8_t *)malloc(len);
    if(buf == NULL) {
        nvs_close(h);
        return false;
    }
    esp_err_t err = nvs_get_blob(h, key, buf, &len);
    nvs_close(h);
    if(err != ESP_OK) {
        free(buf);
        return false;
    }

    bool ok = false;
    dash_cache_hdr_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if(hdr.magic == DASH_CACHE_MAGIC && hdr.version == DASH_CACHE_VER &&
       hdr.size == expect_size && len == sizeof(hdr) + expect_size) {
        memcpy(out, buf + sizeof(hdr), expect_size);
        ok = true;
    } else {
        ESP_LOGW(TAG, "缓存 %s 版本/长度对不上, 当作没有", key);
    }
    free(buf);
    return ok;
}

/* ---------------- 天气 ---------------- */

void dash_cache_save_weather(const dash_weather_data_t * w)
{
    dash_cache_write("c_weather", w, sizeof(*w));
}

bool dash_cache_load_weather(dash_weather_data_t * w)
{
    return dash_cache_read("c_weather", w, sizeof(*w));
}

/* ---------------- 股票 ---------------- */

void dash_cache_save_stocks(const dash_quote_t * stocks, int count)
{
    if(stocks == NULL || count <= 0) return;
    dash_cache_write("c_stock", stocks, sizeof(dash_quote_t) * (size_t)count);
}

bool dash_cache_load_stocks(dash_quote_t * stocks, int max_count, int * count)
{
    if(stocks == NULL || count == NULL) return false;
    *count = 0;

    nvs_handle_t h;
    if(nvs_open(DASH_CACHE_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    if(nvs_get_blob(h, "c_stock", NULL, &len) != ESP_OK || len < sizeof(dash_cache_hdr_t)) {
        nvs_close(h);
        return false;
    }
    uint8_t * buf = (uint8_t *)malloc(len);
    if(buf == NULL) {
        nvs_close(h);
        return false;
    }
    esp_err_t err = nvs_get_blob(h, "c_stock", buf, &len);
    nvs_close(h);
    if(err != ESP_OK) {
        free(buf);
        return false;
    }

    bool ok = false;
    dash_cache_hdr_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    size_t payload = len - sizeof(hdr);
    if(hdr.magic == DASH_CACHE_MAGIC && hdr.version == DASH_CACHE_VER &&
       hdr.size == payload && payload % sizeof(dash_quote_t) == 0) {
        int n = (int)(payload / sizeof(dash_quote_t));
        if(n > max_count) n = max_count;
        memcpy(stocks, buf + sizeof(hdr), sizeof(dash_quote_t) * (size_t)n);
        *count = n;
        ok = true;
    }
    free(buf);
    return ok;
}

/* ---------------- 服务器状态 ---------------- */

void dash_cache_save_sysstat(const dash_sysstat_t * st)
{
    dash_cache_write("c_sysstat", st, sizeof(*st));
}

bool dash_cache_load_sysstat(dash_sysstat_t * st)
{
    return dash_cache_read("c_sysstat", st, sizeof(*st));
}

/* ---------------- HA 设备状态 ---------------- */

void dash_cache_save_ha(const dash_ha_item_t * items, int count)
{
    if(items == NULL || count <= 0) return;
    dash_cache_write("c_ha", items, sizeof(dash_ha_item_t) * (size_t)count);
}

bool dash_cache_load_ha(dash_ha_item_t * items, int max_count, int * count)
{
    if(items == NULL || count == NULL) return false;
    *count = 0;

    nvs_handle_t h;
    if(nvs_open(DASH_CACHE_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    if(nvs_get_blob(h, "c_ha", NULL, &len) != ESP_OK || len < sizeof(dash_cache_hdr_t)) {
        nvs_close(h);
        return false;
    }
    uint8_t * buf = (uint8_t *)malloc(len);
    if(buf == NULL) {
        nvs_close(h);
        return false;
    }
    esp_err_t err = nvs_get_blob(h, "c_ha", buf, &len);
    nvs_close(h);
    if(err != ESP_OK) {
        free(buf);
        return false;
    }

    bool ok = false;
    dash_cache_hdr_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    size_t payload = len - sizeof(hdr);
    if(hdr.magic == DASH_CACHE_MAGIC && hdr.version == DASH_CACHE_VER &&
       hdr.size == payload && payload % sizeof(dash_ha_item_t) == 0) {
        int n = (int)(payload / sizeof(dash_ha_item_t));
        if(n > max_count) n = max_count;
        memcpy(items, buf + sizeof(hdr), sizeof(dash_ha_item_t) * (size_t)n);
        *count = n;
        ok = true;
    }
    free(buf);
    return ok;
}
