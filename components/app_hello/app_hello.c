// SPDX-License-Identifier: Apache-2.0

#include "app_hello.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "bundle_gsp.h"

/* 家庭看板(Mosaico 版)的 UI 控制器。
 *
 * 分工:
 *   场景 JSON  —— 负责版面、配色、静态文字、控件结构
 *   本文件     —— 负责数据流: 定时刷新 + 点击交互 + 生成式 Setter 写回
 *
 * 真实移植时, 这里换成: WiFi → HTTP 拉 Home Assistant / 天气 / 股票,
 * 拿到值后用 gsp_hello_*_set_text() 写回界面。所有网络/解析都放在
 * 独立任务里, GSP 回调只做短通知, 不要在回调里阻塞。
 */
struct app_hello {
    esp_gsp_handle_t ui;
    uint32_t refresh_count;
    uint32_t tick;
    void *timer;
    esp_gsp_err_t last_error;
};

#define REFRESH_MAX 99
#define CLOCK_TICK_MS 1000

static void remember_error(struct app_hello *state, esp_gsp_err_t err)
{
    if (err != ESP_GSP_OK) {
        state->last_error = err;
    }
}

/* 每秒跑一次: 刷新时钟, 并把"温度"按演示数据缓慢浮动,
 * 用来验证运行期动态绑定(set_text)在真机/模拟器里都生效。 */
static void clock_tick(esp_gsp_handle_t ui, void *ctx)
{
    struct app_hello *state = ctx;
    state->tick++;

    time_t now = time(NULL);
    struct tm local;
    if (localtime_r(&now, &local) != NULL) {
        char clock[8];
        snprintf(clock, sizeof(clock), "%02d:%02d", local.tm_hour, local.tm_min);
        remember_error(state, gsp_hello_clock_set_text(ui, clock));
    }

    char temperature[8];
    snprintf(temperature, sizeof(temperature), "%.1f°", 23.0 + (state->tick % 20) * 0.1);
    remember_error(state, gsp_hello_temperature_set_text(ui, temperature));
}

/* 点击"刷新数据": 计数 +1, 并把湿度/空气/股票换一批演示值。 */
static void on_refresh(esp_gsp_handle_t ui, const esp_gsp_event_t *event, void *ctx)
{
    struct app_hello *state = ctx;
    if (!gsp_hello_event_is_say_hello(event)) {
        return;
    }

    state->refresh_count = (state->refresh_count + 1) % (REFRESH_MAX + 1);

    char count[3];
    snprintf(count, sizeof(count), "%02u", (unsigned)state->refresh_count);
    remember_error(state, gsp_hello_count_set_text(ui, count));

    char humidity[8];
    snprintf(humidity, sizeof(humidity), "%u%%", (unsigned)(40u + (state->refresh_count % 30)));
    remember_error(state, gsp_hello_humidity_set_text(ui, humidity));

    char brightness[8];
    snprintf(brightness, sizeof(brightness), "%u%%", (unsigned)(30u + (state->refresh_count % 70)));
    remember_error(state, gsp_hello_brightness_set_text(ui, brightness));
}

esp_gsp_err_t app_hello_create(esp_gsp_handle_t ui, app_hello_handle_t *out_handle)
{
    if (!out_handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    if (!ui) {
        return ESP_GSP_ERR_INVALID_ARG;
    }

    struct app_hello *state = calloc(1, sizeof(*state));
    if (!state) {
        return ESP_GSP_ERR_NO_MEM;
    }
    state->ui = ui;

    remember_error(state, gsp_hello_count_set_text(ui, "00"));
    remember_error(state, gsp_hello_title_set_text(ui, "家庭看板"));
    remember_error(state, gsp_hello_condition_set_text(ui, "多云"));
    remember_error(state, gsp_hello_air_set_text(ui, "良"));
    remember_error(state, gsp_hello_stock_set_text(ui, "+1.2%"));

    esp_gsp_err_t err = state->last_error;
    if (err == ESP_GSP_OK) {
        /* 先注册回调再起定时器: 反过来可能出现"定时器已跑但回调还没挂上"的窗口。 */
        err = esp_gsp_on_event(ui, on_refresh, state);
    }
    if (err == ESP_GSP_OK) {
        state->timer = esp_gsp_timer_create(ui, CLOCK_TICK_MS, clock_tick, state);
        if (!state->timer) {
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

    *out_handle = state;
    return ESP_GSP_OK;
}

esp_gsp_err_t app_hello_read(app_hello_handle_t handle, app_hello_snapshot_t *out_snapshot)
{
    if (!handle || !out_snapshot) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out_snapshot = (app_hello_snapshot_t) {
        .refresh_count = handle->refresh_count,
        .tick = handle->tick,
        .last_error = handle->last_error,
    };
    return ESP_GSP_OK;
}

esp_gsp_err_t app_hello_destroy(app_hello_handle_t *handle)
{
    if (!handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    struct app_hello *state = *handle;
    if (!state) {
        return ESP_GSP_OK;
    }
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
