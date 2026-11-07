/**
 * @file dash_data.c
 * 看板数据层 —— 统一入口(快照 + 通知)的实现
 *
 * 一个后台任务负责天气/股票/服务器状态, 各自按自己的间隔拉;
 * HA 是独立任务(控制命令要能插队);
 * 配置下发在开机后和每次 WiFi 重连后各做一次。
 *
 * 任务只往快照里写, 写完再回调上层 —— 上层拿到的永远是一份自洽的快照,
 * 不会出现"天气是新的、股票是旧的"这种半新半旧的状态。
 */

#include "dash_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "dash_cache.h"
#include "dash_cfg.h"
#include "dash_ha.h"
#include "dash_http.h"
#include "dash_lock.h"
#include "dash_stock.h"
#include "dash_sysstat.h"
#include "dash_weather.h"

static const char *TAG = "dash_data";

/* 各类数据的刷新间隔(毫秒) */
#define IV_CONFIG    300000UL   /* 配置下发 5 分钟 */
#define IV_WEATHER   600000UL   /* 实时天气 10 分钟 */
#define IV_FORECAST 1800000UL  /* 三日预报 30 分钟 */
#define IV_AIR       1800000UL  /* 空气质量 30 分钟 */
#define IV_STOCK       60000UL  /* 股票 1 分钟 */
#define IV_SYSSTAT     15000UL  /* 服务器状态 15 秒 */

static dash_snapshot_t s_snap;
static dash_lock_t    s_snap_lock;

static dash_data_notify_fn s_notify = NULL;
static void                *s_notify_ctx = NULL;

static volatile bool s_running = false;
static volatile bool s_force_refresh = false;
static volatile bool s_net_reconnect = false;
static volatile bool s_net_down = false;
static TaskHandle_t  s_task = NULL;

/* 统计 */
static int s_weather_ok = 0, s_weather_err = 0;
static int s_stock_ok = 0,   s_stock_err = 0;
static int s_sysstat_ok = 0, s_sysstat_err = 0;
static int s_config_ok = 0,  s_config_err = 0;

/* 天气三路各自的"陈旧"标志: 任意一路在用旧数据, 天气整体就算陈旧 */
static bool s_stale_now = false;
static bool s_stale_fc  = false;
static bool s_stale_air = false;
static bool s_stale_stock = false;
static bool s_stale_sysstat = false;

/* ---------------- WiFi 事件 ----------------
 *
 * 只置标志, 真正的活儿留给后台任务(不在事件回调里发 HTTP)。
 */
static void net_ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == IP_EVENT && (id == IP_EVENT_STA_GOT_IP || id == IP_EVENT_ETH_GOT_IP)) {
        s_net_down = false;
        s_net_reconnect = true;
    }
}

static void net_wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_net_down = true;
    }
}

/* ---------------- 快照 ---------------- */

/* 天气三路任意一路在用旧数据, 整块天气就该变淡 */
static bool weather_is_stale(void)
{
    return s_net_down || s_stale_now || s_stale_fc || s_stale_air;
}

/* 把当前状态收进快照, 填好陈旧标志 */
static void snapshot_build(dash_snapshot_t *out)
{
    dash_lock_enter(&s_snap_lock);

    out->weather = s_snap.weather;
    out->weather_valid = s_snap.weather_valid;
    out->weather_stale = weather_is_stale();

    memcpy(out->stocks, s_snap.stocks, sizeof(out->stocks));
    out->stock_count = s_snap.stock_count;
    out->stock_stale = s_stale_stock;

    out->sysstat = s_snap.sysstat;
    out->sysstat_valid = s_snap.sysstat_valid;
    out->sysstat_stale = s_stale_sysstat;

    dash_lock_leave(&s_snap_lock);

    /* HA 状态由它自己的任务维护, 这里单独取一次 */
    out->device_count = dash_ha_snapshot(out->devices, DASH_DEVICE_MAX);
    dash_ha_status_t ha;
    dash_ha_get_status(&ha);
    out->device_stale = ha.stale;

    out->offline = s_net_down || out->weather_stale || out->stock_stale ||
                   out->sysstat_stale || ha.offline;
    snprintf(out->title, sizeof(out->title), "%s", dash_cfg_get()->title);
}

static void notify_snapshot(void)
{
    if (s_notify == NULL) {
        return;
    }
    dash_snapshot_t snap;
    snapshot_build(&snap);
    s_notify(&snap, s_notify_ctx);
}

/* ---------------- 各路数据 ---------------- */

static void fetch_weather_now(void)
{
    dash_weather_data_t w;
    memset(&w, 0, sizeof(w));

    int rc = dash_fetch_weather_now(&w);
    dash_lock_enter(&s_snap_lock);
    if (rc == 0) {
        /* 只覆盖实时那几项, 预报/空气的数据不受影响 */
        snprintf(s_snap.weather.temp, sizeof(s_snap.weather.temp), "%s", w.temp);
        snprintf(s_snap.weather.desc, sizeof(s_snap.weather.desc), "%s", w.desc);
        s_snap.weather_valid = true;
        s_stale_now = false;
        s_weather_ok++;
    } else {
        s_stale_now = true;
        s_weather_err++;
        ESP_LOGW(TAG, "实时天气失败: %s", dash_weather_last_error());
    }
    dash_lock_leave(&s_snap_lock);

    if (rc == 0) {
        dash_cache_save_weather(&s_snap.weather);
    }
}

static void fetch_weather_forecast(void)
{
    dash_weather_data_t w;
    memset(&w, 0, sizeof(w));

    int rc = dash_fetch_weather_forecast(&w);
    dash_lock_enter(&s_snap_lock);
    if (rc == 0) {
        memcpy(s_snap.weather.high, w.high, sizeof(w.high));
        memcpy(s_snap.weather.low, w.low, sizeof(w.low));
        memcpy(s_snap.weather.sunrise, w.sunrise, sizeof(w.sunrise));
        memcpy(s_snap.weather.sunset, w.sunset, sizeof(w.sunset));
        memcpy(s_snap.weather.forecast, w.forecast, sizeof(w.forecast));
        s_stale_fc = false;
    } else {
        s_stale_fc = true;
        ESP_LOGW(TAG, "预报失败: %s", dash_weather_last_error());
    }
    dash_lock_leave(&s_snap_lock);
}

static void fetch_weather_air(void)
{
    dash_weather_data_t w;
    memset(&w, 0, sizeof(w));

    int rc = dash_fetch_weather_air(&w);
    dash_lock_enter(&s_snap_lock);
    if (rc == 0) {
        memcpy(s_snap.weather.aqi, w.aqi, sizeof(w.aqi));
        memcpy(s_snap.weather.aqi_level, w.aqi_level, sizeof(w.aqi_level));
        s_stale_air = false;
    } else {
        s_stale_air = true;
        ESP_LOGW(TAG, "空气质量失败: %s", dash_weather_air_error());
    }
    dash_lock_leave(&s_snap_lock);
}

static void fetch_stocks(void)
{
    dash_quote_t quotes[DASH_STOCK_MAX];
    int n = dash_fetch_stocks(quotes, DASH_STOCK_MAX);
    dash_lock_enter(&s_snap_lock);
    if (n > 0) {
        memcpy(s_snap.stocks, quotes, sizeof(s_snap.stocks));
        s_snap.stock_count = n;
        s_stale_stock = false;
        s_stock_ok++;
    } else {
        s_stale_stock = true;
        s_stock_err++;
    }
    dash_lock_leave(&s_snap_lock);

    if (n > 0) {
        dash_cache_save_stocks(quotes, n);
    }
}

static void fetch_sysstat(void)
{
    dash_sysstat_t st;
    int rc = dash_fetch_sysstat(&st);
    dash_lock_enter(&s_snap_lock);
    if (rc == 0) {
        s_snap.sysstat = st;
        s_snap.sysstat_valid = true;
        s_stale_sysstat = false;
        s_sysstat_ok++;
    } else {
        s_stale_sysstat = true;
        s_sysstat_err++;
    }
    dash_lock_leave(&s_snap_lock);

    if (rc == 0) {
        dash_cache_save_sysstat(&st);
    } else {
        /* 拉不到就用上次的缓存顶着, 界面上标成陈旧 */
        dash_sysstat_t cached;
        if (dash_cache_load_sysstat(&cached)) {
            dash_lock_enter(&s_snap_lock);
            s_snap.sysstat = cached;
            s_snap.sysstat_valid = true;
            dash_lock_leave(&s_snap_lock);
        }
    }
}

/* 从服务器拉一份配置下来。拿到新配置后要把 HA 地址/令牌和服务器地址重新接线。 */
static void fetch_config(void)
{
    esp_err_t err = dash_cfg_fetch(NULL);
    dash_lock_enter(&s_snap_lock);
    if (err == ESP_OK) {
        s_config_ok++;
        dash_lock_leave(&s_snap_lock);

        dash_ha_reload_config();
        dash_sysstat_set_server_base(dash_cfg_server_base());
        ESP_LOGI(TAG, "配置已更新(标题=%s, 设备=%d 台)",
                 dash_cfg_get()->title, dash_cfg_get()->device_count);
    } else {
        s_config_err++;
        dash_lock_leave(&s_snap_lock);
        ESP_LOGW(TAG, "配置下发失败, 继续用本地缓存");
    }
}

/* ---------------- 后台任务 ---------------- */

static void data_task(void *arg)
{
    (void)arg;

    /* 开机先用 NVS 缓存把界面填上, 别让用户对着空白屏幕等 */
    dash_cache_load_weather(&s_snap.weather);
    s_snap.weather_valid = s_snap.weather.temp[0] != 0;
    int cached_stocks = 0;
    if (dash_cache_load_stocks(s_snap.stocks, DASH_STOCK_MAX, &cached_stocks)) {
        s_snap.stock_count = cached_stocks;
    }
    if (dash_cache_load_sysstat(&s_snap.sysstat)) {
        s_snap.sysstat_valid = true;
    }
    s_stale_now = s_stale_fc = s_stale_air = true;
    s_stale_stock = s_stale_sysstat = true;
    notify_snapshot();

    /* 等网络起来 */
    int wait_s = 0;
    while (s_running && wait_s < 60 && dash_cfg_server_base()[0] == 0) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        wait_s++;
    }

    /* 设备清单要先拿到才能起 HA 任务 */
    fetch_config();

    const dash_config_t *cfg = dash_cfg_get();
    if (cfg->device_count > 0) {
        dash_ha_start(cfg->devices, cfg->device_count);
    }
    dash_sysstat_set_server_base(dash_cfg_server_base());

    /* 第一轮: 什么都拉一次 */
    fetch_weather_now();
    fetch_weather_forecast();
    fetch_weather_air();
    fetch_stocks();
    fetch_sysstat();
    notify_snapshot();

    uint32_t t_cfg = esp_timer_get_time() / 1000;
    uint32_t t_now = t_cfg;
    uint32_t t_fc  = t_cfg;
    uint32_t t_air = t_cfg;
    uint32_t t_stock = t_cfg;
    uint32_t t_stat = t_cfg;

    while (s_running) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t now = esp_timer_get_time() / 1000;

        /* WiFi 重连成功 -> 立刻全量刷新一遍 */
        if (s_net_reconnect) {
            s_net_reconnect = false;
            s_net_down = false;
            ESP_LOGI(TAG, "网络恢复, 立刻全量刷新");
            fetch_config();
            fetch_weather_now();
            fetch_weather_forecast();
            fetch_weather_air();
            fetch_stocks();
            fetch_sysstat();
            t_cfg = t_now = t_fc = t_air = t_stock = t_stat = now;
            notify_snapshot();
            continue;
        }

        /* 手动刷新 */
        if (s_force_refresh) {
            s_force_refresh = false;
            fetch_config();
            fetch_weather_now();
            fetch_weather_forecast();
            fetch_weather_air();
            fetch_stocks();
            fetch_sysstat();
            t_cfg = t_now = t_fc = t_air = t_stock = t_stat = now;
            notify_snapshot();
            continue;
        }

        bool changed = false;
        if (now - t_cfg >= IV_CONFIG) {
            t_cfg = now;
            fetch_config();
            changed = true;
        }
        if (now - t_now >= IV_WEATHER) {
            t_now = now;
            fetch_weather_now();
            changed = true;
        }
        if (now - t_fc >= IV_FORECAST) {
            t_fc = now;
            fetch_weather_forecast();
            changed = true;
        }
        if (now - t_air >= IV_AIR) {
            t_air = now;
            fetch_weather_air();
            changed = true;
        }
        if (now - t_stock >= IV_STOCK) {
            t_stock = now;
            fetch_stocks();
            changed = true;
        }
        if (now - t_stat >= IV_SYSSTAT) {
            t_stat = now;
            fetch_sysstat();
            changed = true;
        }

        if (changed) {
            notify_snapshot();
        }
    }

    ESP_LOGI(TAG, "数据层任务退出");
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---------------- 对外接口 ---------------- */

int dash_data_start(dash_data_notify_fn notify, void *ctx)
{
    if (s_running) {
        return ESP_OK;
    }

    dash_lock_init(&s_snap_lock);
    memset(&s_snap, 0, sizeof(s_snap));

    s_notify = notify;
    s_notify_ctx = ctx;

    /* 配置层要先起来: 数据层要靠它拿服务器地址和设备清单 */
    dash_cfg_init();

    /* 监听网络状态(只置标志, 不在回调里发 HTTP) */
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, net_ip_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, net_ip_event_handler, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, net_wifi_event_handler, NULL);

    s_running = true;
    if (xTaskCreate(data_task, "dash_data", 12288, NULL, 5, &s_task) != pdPASS) {
        s_running = false;
        s_task = NULL;
        ESP_LOGE(TAG, "数据层任务创建失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "数据层已启动");
    return ESP_OK;
}

int dash_data_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }
    s_running = false;
    dash_http_set_abort(true);
    dash_ha_stop();

    for (int i = 0; i < 100 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_task = NULL;

    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, net_ip_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, net_ip_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, net_wifi_event_handler);

    dash_http_set_abort(false);
    s_notify = NULL;
    s_notify_ctx = NULL;
    return ESP_OK;
}

bool dash_data_get_snapshot(dash_snapshot_t *out)
{
    if (out == NULL) {
        return false;
    }
    snapshot_build(out);
    return true;
}

int dash_data_set_device(int index, int32_t value)
{
    if (index < 0 || index >= DASH_DEVICE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    const dash_config_t *cfg = dash_cfg_get();
    if (index >= cfg->device_count) {
        return ESP_ERR_NOT_FOUND;
    }
    const dash_ha_device_t *dev = &cfg->devices[index];
    dash_ha_request(dev->entity_id, dev->domain, value);
    return ESP_OK;
}

int dash_data_refresh(void)
{
    s_force_refresh = true;
    return ESP_OK;
}

void dash_data_get_diag(dash_data_diag_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->weather_ok = s_weather_ok;
    out->weather_err = s_weather_err;
    out->stock_ok = s_stock_ok;
    out->stock_err = s_stock_err;
    out->sysstat_ok = s_sysstat_ok;
    out->sysstat_err = s_sysstat_err;
    out->config_ok = s_config_ok;
    out->config_err = s_config_err;
    out->offline = s_net_down;
    snprintf(out->err, sizeof(out->err), "%s", dash_weather_last_error());
}
