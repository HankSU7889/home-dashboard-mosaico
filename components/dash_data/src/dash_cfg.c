/**
 * @file dash_cfg.c
 * 看板数据层 —— 运行时配置(服务器下发)的实现
 */

#include "dash_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "dash_http.h"
#include "mosaico_shared.h"

static const char *TAG = "dash_cfg";

/* NVS 里存的键 */
#define K_SERVER_BASE "base"
#define K_DEVICE_ID   "device"
#define K_DEVS        "devs"

/* 看板服务器上配置接口的路径 */
#define CFG_PATH "/device/config"

static dash_config_t s_cfg;
static bool s_cfg_ready = false;

/* ---------------- NVS ---------------- */

/* 从 NVS 读一个字符串, 读不到就保留原值 */
static void nvs_get_str_or_keep(nvs_handle_t h, const char *key, char *out, size_t out_len)
{
    size_t len = out_len;
    if (nvs_get_str(h, key, out, &len) == ESP_OK) {
        out[out_len - 1] = 0;
    }
}

/* ---------------------------------------------------------------------------
 * 落盘格式: 每个字段一个 NVS 键
 *
 * 早先是把所有字段用 '|' 拼成一个大串存一个键 —— 一是串太长容易溢出,
 * 二是字段里只要带了 '|' 就会串台(HA 令牌、URL 都可能带)。现在一个字段一个键,
 * 长度天然受字段本身数组大小的约束, 不用再担心拼串溢出。
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *key; /**< NVS 键 */
    size_t      off; /**< 字段在 dash_config_t 里的偏移 */
    size_t      len; /**< 字段长度(含结尾的 '\0') */
} cfg_field_t;

#define CFG_FIELD_LEN(field) (sizeof(((dash_config_t *)0)->field))

static const cfg_field_t CFG_FIELDS[] = {
    { "title",     offsetof(dash_config_t, title),             CFG_FIELD_LEN(title) },
    { "haurl",     offsetof(dash_config_t, ha_url),            CFG_FIELD_LEN(ha_url) },
    { "hatoken",   offsetof(dash_config_t, ha_token),          CFG_FIELD_LEN(ha_token) },
    { "qwkey",     offsetof(dash_config_t, qweather_key),      CFG_FIELD_LEN(qweather_key) },
    { "qwhost",    offsetof(dash_config_t, qweather_host),     CFG_FIELD_LEN(qweather_host) },
    { "qwloc",     offsetof(dash_config_t, qweather_location), CFG_FIELD_LEN(qweather_location) },
    { "codes",     offsetof(dash_config_t, stock_codes),       CFG_FIELD_LEN(stock_codes) },
    { "names",     offsetof(dash_config_t, stock_names),       CFG_FIELD_LEN(stock_names) },
};

#define CFG_FIELD_COUNT (sizeof(CFG_FIELDS) / sizeof(CFG_FIELDS[0]))

/* 设备清单一条记录的字节数: 显示名 + 实体 ID + 域 */
#define DEV_REC_SIZE (sizeof(((dash_ha_device_t *)0)->name) + \
                      sizeof(((dash_ha_device_t *)0)->entity_id) + 1)

/* 把配置写进 NVS(只写敏感字段和需要跨设备区分的字段) */
static void cfg_save_nvs(nvs_handle_t h, const dash_config_t *cfg)
{
    for (size_t i = 0; i < CFG_FIELD_COUNT; i++) {
        nvs_set_str(h, CFG_FIELDS[i].key, (const char *)cfg + CFG_FIELDS[i].off);
    }

    /* 设备清单用定长 blob 存, 每条 DEV_REC_SIZE 字节, 不用分隔符 */
    uint8_t blob[DASH_DEVICE_MAX * DEV_REC_SIZE];
    memset(blob, 0, sizeof(blob));
    int count = cfg->device_count;
    if (count > DASH_DEVICE_MAX) {
        count = DASH_DEVICE_MAX;
    }
    for (int i = 0; i < count; i++) {
        uint8_t *p = blob + i * DEV_REC_SIZE;
        memcpy(p, cfg->devices[i].name, sizeof(cfg->devices[i].name));
        memcpy(p + sizeof(cfg->devices[i].name),
               cfg->devices[i].entity_id, sizeof(cfg->devices[i].entity_id));
        p[DEV_REC_SIZE - 1] = cfg->devices[i].domain;
    }
    nvs_set_blob(h, K_DEVS, blob, sizeof(blob));
}

/* 从 NVS 把配置读回来; 读不到任何东西返回 false */
static bool cfg_load_nvs(nvs_handle_t h, dash_config_t *cfg)
{
    bool any = false;
    for (size_t i = 0; i < CFG_FIELD_COUNT; i++) {
        char *dst = (char *)cfg + CFG_FIELDS[i].off;
        size_t len = CFG_FIELDS[i].len;
        if (nvs_get_str(h, CFG_FIELDS[i].key, dst, &len) == ESP_OK) {
            dst[CFG_FIELDS[i].len - 1] = 0;
            any = true;
        }
    }

    uint8_t blob[DASH_DEVICE_MAX * DEV_REC_SIZE];
    size_t blob_len = sizeof(blob);
    if (nvs_get_blob(h, K_DEVS, blob, &blob_len) == ESP_OK) {
        int count = (int)(blob_len / DEV_REC_SIZE);
        if (count > DASH_DEVICE_MAX) {
            count = DASH_DEVICE_MAX;
        }
        for (int i = 0; i < count; i++) {
            const uint8_t *p = blob + i * DEV_REC_SIZE;
            memcpy(cfg->devices[i].name, p, sizeof(cfg->devices[i].name));
            memcpy(cfg->devices[i].entity_id, p + sizeof(cfg->devices[i].name),
                   sizeof(cfg->devices[i].entity_id));
            cfg->devices[i].domain = p[DEV_REC_SIZE - 1];
        }
        cfg->device_count = count;
        any = true;
    }
    return any;
}

/* ---------------- 身份 ---------------- */

/* 用 STA MAC 后三字节做设备身份; 拿不到就退回一个固定串 */
static void default_device_id(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(out, out_len, "%02x%02x%02x", mac[3], mac[4], mac[5]);
        return;
    }
    snprintf(out, out_len, "default");
}

/* ---------------- 解析服务器下发的 JSON ---------------- */

static void json_copy(cJSON *obj, const char *key, char *out, size_t out_len)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item == NULL || !cJSON_IsString(item) || item->valuestring == NULL) {
        return;
    }
    snprintf(out, out_len, "%s", item->valuestring);
}

/* 域名字符串转成 DASH_HA_* 常量; 认不出来就按灯处理 */
static uint8_t domain_from_string(const char *s)
{
    if (s != NULL && strcmp(s, "fan") == 0) {
        return DASH_HA_FAN;
    }
    return DASH_HA_LIGHT;
}

static bool parse_config_json(const char *json, dash_config_t *cfg)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGW(TAG, "配置 JSON 解析失败");
        return false;
    }

    json_copy(root, "title", cfg->title, sizeof(cfg->title));
    json_copy(root, "ha_url", cfg->ha_url, sizeof(cfg->ha_url));
    json_copy(root, "ha_token", cfg->ha_token, sizeof(cfg->ha_token));
    json_copy(root, "qweather_key", cfg->qweather_key, sizeof(cfg->qweather_key));
    json_copy(root, "qweather_host", cfg->qweather_host, sizeof(cfg->qweather_host));
    json_copy(root, "qweather_location", cfg->qweather_location, sizeof(cfg->qweather_location));
    json_copy(root, "stock_codes", cfg->stock_codes, sizeof(cfg->stock_codes));
    json_copy(root, "stock_names", cfg->stock_names, sizeof(cfg->stock_names));

    /* 设备清单 */
    cfg->device_count = 0;
    cJSON *devices = cJSON_GetObjectItem(root, "devices");
    cJSON *dev = NULL;
    cJSON_ArrayForEach(dev, devices) {
        if (cfg->device_count >= DASH_DEVICE_MAX) {
            break;
        }
        dash_ha_device_t *dst = &cfg->devices[cfg->device_count];
        memset(dst, 0, sizeof(*dst));
        json_copy(dev, "name", dst->name, sizeof(dst->name));
        json_copy(dev, "entity", dst->entity_id, sizeof(dst->entity_id));
        char domain[16] = {0};
        json_copy(dev, "domain", domain, sizeof(domain));
        dst->domain = domain_from_string(domain);
        if (dst->entity_id[0] == 0) {
            continue;   /* 没有实体 ID 的条目直接丢掉 */
        }
        cfg->device_count++;
    }

    cJSON_Delete(root);
    return true;
}

/* ---------------- 对外接口 ---------------- */

int dash_cfg_apply(const dash_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg;
    if (s_cfg.title[0] == 0) {
        snprintf(s_cfg.title, sizeof(s_cfg.title), "%s", DASH_CFG_DEFAULT_TITLE);
    }
    s_cfg_ready = true;

    /* 落盘: 敏感的字段留在 NVS, 不进固件, 不进 git */
    nvs_handle_t h;
    esp_err_t err = nvs_open(DASH_CFG_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 打开失败, 配置不会持久化 (0x%x)", err);
        return err;
    }
    nvs_set_str(h, K_DEVICE_ID, s_cfg.device_id);
    nvs_set_str(h, K_SERVER_BASE, s_cfg.server_base);
    cfg_save_nvs(h, &s_cfg);
    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "配置已保存 (标题=%s, 设备=%d 台, 天气key=%s)",
                 s_cfg.title, s_cfg.device_count,
                 s_cfg.qweather_key[0] ? "有" : "无");
    }
    return err;
}

int dash_cfg_fetch(dash_config_t *out)
{
    if (!s_cfg_ready) {
        dash_cfg_init();
    }
    if (s_cfg.server_base[0] == 0) {
        ESP_LOGW(TAG, "还没有服务器地址, 跳过配置下发");
        return ESP_ERR_INVALID_STATE;
    }

    char url[320];
    snprintf(url, sizeof(url), "%s%s?device_id=%s",
             s_cfg.server_base, CFG_PATH, s_cfg.device_id);

    char *buf = (char *)malloc(DASH_HTTP_BODY_MAX);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int n = dash_http_get(url, buf, DASH_HTTP_BODY_MAX, DASH_HTTP_TIMEOUT_MS, 1);
    if (n <= 0) {
        free(buf);
        ESP_LOGW(TAG, "配置下发失败: %s", dash_http_last_error());
        return ESP_FAIL;
    }

    dash_config_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.server_base[0] = 0;
    snprintf(fresh.server_base, sizeof(fresh.server_base), "%s", s_cfg.server_base);
    snprintf(fresh.device_id, sizeof(fresh.device_id), "%s", s_cfg.device_id);

    if (!parse_config_json(buf, &fresh)) {
        free(buf);
        return ESP_ERR_INVALID_RESPONSE;
    }
    free(buf);

    esp_err_t err = dash_cfg_apply(&fresh);
    if (err == ESP_OK && out != NULL) {
        *out = fresh;
    }
    return err;
}

const dash_config_t *dash_cfg_get(void)
{
    return &s_cfg;
}

const char *dash_cfg_server_base(void)
{
    return s_cfg.server_base;
}

const char *dash_cfg_device_id(void)
{
    return s_cfg.device_id;
}

int dash_cfg_init(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    snprintf(s_cfg.title, sizeof(s_cfg.title), "%s", DASH_CFG_DEFAULT_TITLE);
    default_device_id(s_cfg.device_id, sizeof(s_cfg.device_id));

    /* 1) 先从 NVS 读上一次的结果 */
    nvs_handle_t h;
    if (nvs_open(DASH_CFG_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        dash_config_t cached;
        memset(&cached, 0, sizeof(cached));
        if (cfg_load_nvs(h, &cached)) {
            /* 服务器地址和设备身份以 NVS 里单独存的那两份为准 */
            nvs_get_str_or_keep(h, K_SERVER_BASE, cached.server_base, sizeof(cached.server_base));
            nvs_get_str_or_keep(h, K_DEVICE_ID, cached.device_id, sizeof(cached.device_id));
            if (cached.device_id[0] == 0) {
                snprintf(cached.device_id, sizeof(cached.device_id), "%s", s_cfg.device_id);
            }
            if (cached.title[0] == 0) {
                snprintf(cached.title, sizeof(cached.title), "%s", DASH_CFG_DEFAULT_TITLE);
            }
            s_cfg = cached;
        }
        nvs_close(h);
    }

    /* 2) Vibe Mode 共享信息里的服务器地址优先(用户在设备屏幕上直接填) */
    char shared[128] = {0};
    if (mosaico_shared_get("dash_server", shared, sizeof(shared)) == ESP_OK && shared[0] != 0) {
        snprintf(s_cfg.server_base, sizeof(s_cfg.server_base), "%s", shared);
        /* 去掉末尾多余的 '/', 免得拼出 "//device/config" */
        int n = strlen(s_cfg.server_base);
        while (n > 0 && s_cfg.server_base[n - 1] == '/') {
            s_cfg.server_base[--n] = 0;
        }
    }

    /* 3) 设备身份也可以在共享信息里覆盖 */
    char dev_id[32] = {0};
    if (mosaico_shared_get("dash_device", dev_id, sizeof(dev_id)) == ESP_OK && dev_id[0] != 0) {
        snprintf(s_cfg.device_id, sizeof(s_cfg.device_id), "%s", dev_id);
    }

    s_cfg_ready = true;
    ESP_LOGI(TAG, "配置初始化: 身份=%s 服务器=%s",
             s_cfg.device_id, s_cfg.server_base[0] ? s_cfg.server_base : "(未配置)");
    return ESP_OK;
}
