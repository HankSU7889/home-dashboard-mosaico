// SPDX-License-Identifier: Apache-2.0

/**
 * @file main.c
 * 家庭看板(Mosaico 版) —— 组装根
 *
 * 启动顺序有讲究:
 *   1. Iris 启动前先探崩溃, 免得应用初始化崩了进不去 Recovery
 *   2. 网络栈先建起来(配没配网都要建), 后面 ESP-Iris 的 TCP 通道依赖它
 *   3. Iris 起来之后才有办法远程看日志/装固件
 *   4. GSP 界面起来, 最后把数据层挂上
 */

#include "app_dash.h"
#include "app_display.h"
#include "app_net.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "iris_ota_support.h"
#include "iris_screen_mirror.h"
#include "nvs_flash.h"
#include "ui_bundle.h"

static const char *TAG = "home_dashboard";

/* 组装根只持有拥有的句柄, 不碰控制器内部状态。 */
static struct {
    esp_gsp_handle_t ui;
    app_dash_handle_t controller;
} application;

void app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    ESP_ERROR_CHECK(nvs_flash_init());

    /* 网络栈: 没配网也要建起来, 否则 Iris 的 TCP 通道没有落脚点。
     * 没配网不算致命错误, 界面上会显示"未配网"。 */
    const esp_err_t net_err = app_net_start();
    if (net_err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi 未就绪 (0x%x); 界面会提示未配网", net_err);
    }

    /* Recovery 必须一直可达, 哪怕后面的 UI 起不来。 */
    iris_ota_support_start();

    esp_gsp_config_t config;
    const esp_err_t err = ui_bundle_open(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GSP UI unavailable (0x%x); Recovery RPC remains active", err);
        return;
    }
    esp_gsp_esp_lcd_config_t display;
    const esp_err_t display_err = app_display_init(&display);
    if (display_err != ESP_OK) {
        ESP_LOGE(TAG, "Display unavailable (0x%x); Recovery RPC remains active", display_err);
        return;
    }
    ESP_ERROR_CHECK(iris_screen_mirror_init());
    ESP_ERROR_CHECK(esp_gsp_esp_lcd_start(&config, &display, &application.ui));
    ESP_ERROR_CHECK(iris_screen_mirror_attach(application.ui));
    ESP_ERROR_CHECK(app_dash_create(application.ui, &application.controller));
    /* GSP 持有回调上下文。设备 UI/控制器活到重启为止;
     * 拆除时要先让 GSP 停止派发, 再 app_dash_destroy(), 最后关 UI。 */
    ESP_LOGI(TAG, "家庭看板已启动");
}
