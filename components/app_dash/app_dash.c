// SPDX-License-Identifier: Apache-2.0

/**
 * @file app_dash.c
 * 家庭看板(Mosaico 版)的 UI 控制器
 *
 * 三条线:
 *   1) 定时器(1 秒)  —— 刷时钟、跟页码
 *   2) 数据回调      —— dash_data 每拿到一份新快照就调这里, 把值写进界面
 *   3) 事件回调      —— 刷新按钮、五个设备滑块
 *
 * 所有 setter 都是往 GSP 渲染任务队列里投命令, 不阻塞, 所以在数据层任务里调也安全。
 * 但回调本身不能做耗时的事: 网络请求都在 dash_data 里, 这里只做"排队写界面"。
 */

#include "app_dash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bundle_gsp.h"
#include "dash_cfg.h"
#include "dash_data.h"
#include "dash_types.h"

#define CLOCK_TICK_MS 1000
#define PAGE_COUNT     3

/* 温度环的范围: -20 度到 50 度映射到 0~100 */
#define TEMP_MIN (-20)
#define TEMP_MAX (50)

struct app_dash {
    esp_gsp_handle_t ui;
    void *timer;
    esp_gsp_err_t last_error;
    uint32_t tick;
    uint32_t refresh_count;
    uint32_t snapshot_count;
    uint32_t command_count;
    uint32_t last_page;
};

static void remember_error(struct app_dash *state, esp_gsp_err_t err)
{
    if (err != ESP_GSP_OK) {
        state->last_error = err;
    }
}

/* ---------------- 数据 -> 界面 ---------------- */

/* 天气: 温度、天气状况、最高最低、日出、空气、空气质量、三日预报 */
static void apply_weather(struct app_dash *state, const dash_snapshot_t *snap)
{
    const dash_weather_data_t *w = &snap->weather;

    /* 温度: 数据层给的是 "23" 或 "23.5", 这里补上单位 */
    char temp[16];
    snprintf(temp, sizeof(temp), "%s°", w->temp[0] ? w->temp : "--");
    remember_error(state, gsp_dash_temp_value_set_text(state->ui, temp));

    /* 温度环: -20..50 映射到 0..100 */
    float temp_f = w->temp[0] ? (float)atof(w->temp) : 0.0f;
    int ring = (int)((temp_f - (float)TEMP_MIN) * 100.0f / (float)(TEMP_MAX - TEMP_MIN));
    if (ring < 0) {
        ring = 0;
    }
    if (ring > 100) {
        ring = 100;
    }
    remember_error(state, gsp_dash_temp_ring_set_value(state->ui, ring));

    remember_error(state, gsp_dash_condition_set_text(state->ui, w->desc[0] ? w->desc : "--"));

    char high_low[48];
    if (w->high[0] && w->low[0]) {
        snprintf(high_low, sizeof(high_low), "%s° / %s°", w->high, w->low);
    } else {
        snprintf(high_low, sizeof(high_low), "-- / --");
    }
    remember_error(state, gsp_dash_high_low_set_text(state->ui, high_low));

    char humidity[16];
    if (w->humidity[0]) {
        snprintf(humidity, sizeof(humidity), "%s%%", w->humidity);
    } else {
        snprintf(humidity, sizeof(humidity), "--");
    }
    remember_error(state, gsp_dash_humidity_set_text(state->ui, humidity));

    remember_error(state, gsp_dash_air_set_text(state->ui, w->aqi_level[0] ? w->aqi_level : "--"));
    remember_error(state, gsp_dash_sun_set_text(state->ui, w->sunrise[0] ? w->sunrise : "--"));

    char aqi[32];
    if (w->aqi[0]) {
        snprintf(aqi, sizeof(aqi), "%s %s", w->aqi, w->aqi_level);
    } else {
        snprintf(aqi, sizeof(aqi), "--");
    }
    remember_error(state, gsp_dash_aqi_set_text(state->ui, aqi));

    /* 三天预报: 第一条是今天, 界面空间只放得下后两天 */
    char forecast[128];   /* 两条预报各 48 字节, 加上分隔符不够 64 */
    const char *f1 = w->forecast[1][0] ? w->forecast[1] : NULL;
    const char *f2 = w->forecast[2][0] ? w->forecast[2] : NULL;
    if (f1 && f2) {
        snprintf(forecast, sizeof(forecast), "%s · %s", f1, f2);
    } else if (f1) {
        snprintf(forecast, sizeof(forecast), "%s", f1);
    } else {
        snprintf(forecast, sizeof(forecast), "--");
    }
    remember_error(state, gsp_dash_forecast_set_text(state->ui, forecast));

    /* 天气是缓存数据时盖一层灰, 让人一眼看出不是实时的 */
    remember_error(state, gsp_dash_weather_veil_set_visible(state->ui, snap->weather_stale));
}

/* 设备: 名字、当前值、滑块位置 */
static void apply_devices(struct app_dash *state, const dash_snapshot_t *snap)
{
    const dash_config_t *cfg = dash_cfg_get();

    for (int i = 0; i < DASH_DEVICE_MAX; i++) {
        bool have = (i < cfg->device_count) && (i < snap->device_count);

        switch (i) {
        case 0:
            remember_error(state, gsp_dash_dev1_name_set_text(state->ui, have ? cfg->devices[i].name : ""));
            remember_error(state, gsp_dash_dev1_value_set_text(state->ui, ""));
            remember_error(state, gsp_dash_dev1_slider_set_visible(state->ui, have));
            break;
        case 1:
            remember_error(state, gsp_dash_dev2_name_set_text(state->ui, have ? cfg->devices[i].name : ""));
            remember_error(state, gsp_dash_dev2_value_set_text(state->ui, ""));
            remember_error(state, gsp_dash_dev2_slider_set_visible(state->ui, have));
            break;
        case 2:
            remember_error(state, gsp_dash_dev3_name_set_text(state->ui, have ? cfg->devices[i].name : ""));
            remember_error(state, gsp_dash_dev3_value_set_text(state->ui, ""));
            remember_error(state, gsp_dash_dev3_slider_set_visible(state->ui, have));
            break;
        case 3:
            remember_error(state, gsp_dash_dev4_name_set_text(state->ui, have ? cfg->devices[i].name : ""));
            remember_error(state, gsp_dash_dev4_value_set_text(state->ui, ""));
            remember_error(state, gsp_dash_dev4_slider_set_visible(state->ui, have));
            break;
        default:
            remember_error(state, gsp_dash_dev5_name_set_text(state->ui, have ? cfg->devices[i].name : ""));
            remember_error(state, gsp_dash_dev5_value_set_text(state->ui, ""));
            remember_error(state, gsp_dash_dev5_slider_set_visible(state->ui, have));
            break;
        }
        if (!have) {
            continue;
        }

        int value = snap->devices[i].value;
        if (value < 0) {
            value = 0;
        }
        if (value > 100) {
            value = 100;
        }
        char text[8];
        snprintf(text, sizeof(text), "%d%%", value);

        switch (i) {
        case 0:
            remember_error(state, gsp_dash_dev1_value_set_text(state->ui, text));
            remember_error(state, gsp_dash_dev1_slider_set_value(state->ui, value));
            break;
        case 1:
            remember_error(state, gsp_dash_dev2_value_set_text(state->ui, text));
            remember_error(state, gsp_dash_dev2_slider_set_value(state->ui, value));
            break;
        case 2:
            remember_error(state, gsp_dash_dev3_value_set_text(state->ui, text));
            remember_error(state, gsp_dash_dev3_slider_set_value(state->ui, value));
            break;
        case 3:
            remember_error(state, gsp_dash_dev4_value_set_text(state->ui, text));
            remember_error(state, gsp_dash_dev4_slider_set_value(state->ui, value));
            break;
        default:
            remember_error(state, gsp_dash_dev5_value_set_text(state->ui, text));
            remember_error(state, gsp_dash_dev5_slider_set_value(state->ui, value));
            break;
        }
    }
}

/* 股票 + 服务器状态 */
static void apply_data(struct app_dash *state, const dash_snapshot_t *snap)
{
    for (int i = 0; i < DASH_STOCK_MAX; i++) {
        bool have = (i < snap->stock_count);
        const dash_quote_t *q = have ? &snap->stocks[i] : NULL;
        /* 涨绿跌红是中国习惯的反过来? 国内是红涨绿跌, 这里跟 7C 保持一致 */
        const char *arrow = "";
        if (q != NULL) {
            arrow = (q->direction > 0) ? "▲" : ((q->direction < 0) ? "▼" : "");
        }
        char pct[24];
        if (q != NULL && q->pct[0]) {
            snprintf(pct, sizeof(pct), "%s%s", arrow, q->pct);
        } else {
            pct[0] = '\0';
        }

        switch (i) {
        case 0:
            remember_error(state, gsp_dash_stock1_name_set_text(state->ui, q ? q->name : ""));
            remember_error(state, gsp_dash_stock1_price_set_text(state->ui, q ? q->price : ""));
            remember_error(state, gsp_dash_stock1_pct_set_text(state->ui, pct));
            break;
        case 1:
            remember_error(state, gsp_dash_stock2_name_set_text(state->ui, q ? q->name : ""));
            remember_error(state, gsp_dash_stock2_price_set_text(state->ui, q ? q->price : ""));
            remember_error(state, gsp_dash_stock2_pct_set_text(state->ui, pct));
            break;
        case 2:
            remember_error(state, gsp_dash_stock3_name_set_text(state->ui, q ? q->name : ""));
            remember_error(state, gsp_dash_stock3_price_set_text(state->ui, q ? q->price : ""));
            remember_error(state, gsp_dash_stock3_pct_set_text(state->ui, pct));
            break;
        default:
            remember_error(state, gsp_dash_stock4_name_set_text(state->ui, q ? q->name : ""));
            remember_error(state, gsp_dash_stock4_price_set_text(state->ui, q ? q->price : ""));
            remember_error(state, gsp_dash_stock4_pct_set_text(state->ui, pct));
            break;
        }
    }

    const dash_sysstat_t *st = &snap->sysstat;
    for (int i = 0; i < DASH_HOST_MAX; i++) {
        bool have = st->valid && (i < st->host_count);
        char metric[64];
        if (have) {
            snprintf(metric, sizeof(metric), "CPU %.0f%%  内存 %.0f%%",
                     st->hosts[i].cpu_pct, st->hosts[i].mem_pct);
        } else {
            metric[0] = '\0';
        }
        switch (i) {
        case 0:
            remember_error(state, gsp_dash_host1_name_set_text(state->ui, have ? st->hosts[i].name : ""));
            remember_error(state, gsp_dash_host1_metric_set_text(state->ui, metric));
            break;
        case 1:
            remember_error(state, gsp_dash_host2_name_set_text(state->ui, have ? st->hosts[i].name : ""));
            remember_error(state, gsp_dash_host2_metric_set_text(state->ui, metric));
            break;
        default:
            remember_error(state, gsp_dash_host3_name_set_text(state->ui, have ? st->hosts[i].name : ""));
            remember_error(state, gsp_dash_host3_metric_set_text(state->ui, metric));
            break;
        }
    }

    char summary[64];
    if (st->valid) {
        snprintf(summary, sizeof(summary), "负载 %.2f  磁盘 %d%%", st->load, st->disk_pct);
    } else {
        summary[0] = '\0';
    }
    remember_error(state, gsp_dash_sys_summary_set_text(state->ui, summary));

    char summary2[64];
    if (st->valid) {
        snprintf(summary2, sizeof(summary2), "延迟 %dms  下载 %d KB/s", st->rtt_ms, st->down_kbps);
    } else {
        summary2[0] = '\0';
    }
    remember_error(state, gsp_dash_sys_summary2_set_text(state->ui, summary2));

    remember_error(state, gsp_dash_data_veil_set_visible(state->ui, snap->stock_stale && snap->sysstat_stale));
}

/* dash_data 的数据回调: 跑在数据层任务上下文里 */
static void on_snapshot(const dash_snapshot_t *snap, void *ctx)
{
    struct app_dash *state = ctx;
    if (state == NULL || snap == NULL) {
        return;
    }
    state->snapshot_count++;

    remember_error(state, gsp_dash_title_set_text(state->ui, snap->title));

    /* 顶栏状态: 离线 / 缓存 / 在线 */
    const char *net_text = "在线";
    if (snap->offline) {
        net_text = (snap->weather_stale || snap->stock_stale || snap->sysstat_stale) ? "缓存" : "离线";
    }
    remember_error(state, gsp_dash_net_state_set_text(state->ui, net_text));

    apply_weather(state, snap);
    apply_devices(state, snap);
    apply_data(state, snap);
}

/* ---------------- 定时器 ---------------- */

/* 每秒跑一次: 刷时钟, 并把页码指示器跟上当前页 */
static void clock_tick(esp_gsp_handle_t ui, void *ctx)
{
    struct app_dash *state = ctx;
    state->tick++;

    time_t now = time(NULL);
    struct tm local;
    if (localtime_r(&now, &local) != NULL) {
        char clock[8];
        snprintf(clock, sizeof(clock), "%02d:%02d", local.tm_hour, local.tm_min);
        remember_error(state, gsp_dash_clock_set_text(ui, clock));
    }

    uint16_t page = 0;
    if (esp_gsp_page_flow_get_page(ui, GSP_DASH_OBJ_KEY_PAGES, &page) == ESP_GSP_OK) {
        if (page != state->last_page) {
            /* 页码越界就夹回最后一页, 免得指示器显示 "4 / 3" */
            if (page >= PAGE_COUNT) {
                page = PAGE_COUNT - 1;
            }
            state->last_page = page;
            char indicator[12];
            snprintf(indicator, sizeof(indicator), "%u / %d", (unsigned)(page + 1), PAGE_COUNT);
            remember_error(state, gsp_dash_page_indicator_set_text(ui, indicator));
        }
    }
}

/* ---------------- 事件 ---------------- */

/* 五个设备滑块: 松手时把值发给 HA */
static void on_device_change(struct app_dash *state, int index, int32_t value)
{
    if (value < 0) {
        value = 0;
    }
    if (value > 100) {
        value = 100;
    }
    state->command_count++;

    /* 先把界面上的数字跟上, 免得等回读才有反馈 */
    char text[8];
    snprintf(text, sizeof(text), "%d%%", (int)value);
    switch (index) {
    case 0:
        remember_error(state, gsp_dash_dev1_value_set_text(state->ui, text));
        break;
    case 1:
        remember_error(state, gsp_dash_dev2_value_set_text(state->ui, text));
        break;
    case 2:
        remember_error(state, gsp_dash_dev3_value_set_text(state->ui, text));
        break;
    case 3:
        remember_error(state, gsp_dash_dev4_value_set_text(state->ui, text));
        break;
    default:
        remember_error(state, gsp_dash_dev5_value_set_text(state->ui, text));
        break;
    }

    dash_data_set_device(index, value);
}

/* GSP 事件回调: 刷新按钮 + 设备滑块 */
static void on_event(esp_gsp_handle_t ui, const esp_gsp_event_t *event, void *ctx)
{
    struct app_dash *state = ctx;
    (void)ui;

    if (gsp_dash_event_is_refresh(event)) {
        state->refresh_count++;
        dash_data_refresh();
        return;
    }

    gsp_dash_call_event_t call;
    if (!gsp_dash_event_decode_call(event, &call)) {
        return;
    }
    int32_t value = (int32_t)call.arg;
    switch (call.action) {
    case GSP_DASH_ACTION_DEV1_CHANGE:
        on_device_change(state, 0, value);
        break;
    case GSP_DASH_ACTION_DEV2_CHANGE:
        on_device_change(state, 1, value);
        break;
    case GSP_DASH_ACTION_DEV3_CHANGE:
        on_device_change(state, 2, value);
        break;
    case GSP_DASH_ACTION_DEV4_CHANGE:
        on_device_change(state, 3, value);
        break;
    case GSP_DASH_ACTION_DEV5_CHANGE:
        on_device_change(state, 4, value);
        break;
    default:
        break;
    }
}

/* ---------------- 生命周期 ---------------- */

esp_gsp_err_t app_dash_create(esp_gsp_handle_t ui, app_dash_handle_t *out_handle)
{
    if (out_handle == NULL || ui == NULL) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    struct app_dash *state = calloc(1, sizeof(*state));
    if (state == NULL) {
        return ESP_GSP_ERR_NO_MEM;
    }
    state->ui = ui;

    /* 先注册回调再起定时器: 反过来可能出现"定时器已跑但回调还没挂上"的窗口。 */
    esp_gsp_err_t err = esp_gsp_on_event(ui, on_event, state);
    if (err == ESP_GSP_OK) {
        state->timer = esp_gsp_timer_create(ui, CLOCK_TICK_MS, clock_tick, state);
        if (state->timer == NULL) {
            err = ESP_GSP_ERR_NO_MEM;
        }
    }
    if (err != ESP_GSP_OK) {
        if (state->timer) {
            esp_gsp_timer_delete(ui, state->timer);
            state->timer = NULL;
        }
        esp_gsp_on_event(ui, NULL, NULL);
        free(state);
        return err;
    }

    /* 数据层起来之后会推第一份快照过来 */
    err = dash_data_start(on_snapshot, state);
    if (err != ESP_OK) {
        esp_gsp_timer_delete(ui, state->timer);
        esp_gsp_on_event(ui, NULL, NULL);
        free(state);
        return ESP_GSP_ERR_INVALID_STATE;
    }

    *out_handle = state;
    return ESP_GSP_OK;
}

esp_gsp_err_t app_dash_read(app_dash_handle_t handle, app_dash_snapshot_t *out_snapshot)
{
    if (handle == NULL || out_snapshot == NULL) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out_snapshot = (app_dash_snapshot_t) {
        .tick = handle->tick,
        .refresh_count = handle->refresh_count,
        .snapshot_count = handle->snapshot_count,
        .command_count = handle->command_count,
        .last_page = handle->last_page,
        .last_error = handle->last_error,
    };
    return ESP_GSP_OK;
}

esp_gsp_err_t app_dash_destroy(app_dash_handle_t *handle)
{
    if (handle == NULL) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    struct app_dash *state = *handle;
    if (state == NULL) {
        return ESP_GSP_OK;
    }

    /* 先停数据层: 它还可能往这个控制器上推快照 */
    dash_data_stop();

    esp_gsp_err_t err = esp_gsp_on_event(state->ui, NULL, NULL);
    if (err != ESP_GSP_OK) {
        return err;
    }
    if (state->timer) {
        err = esp_gsp_timer_delete(state->ui, state->timer);
        if (err != ESP_GSP_OK) {
            return err;
        }
    }
    free(state);
    *handle = NULL;
    return ESP_GSP_OK;
}
