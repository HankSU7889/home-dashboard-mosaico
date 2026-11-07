// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>

#include "app_dash.h"
#include "gsp_sim_bridge.h"

/* 模拟器后端: 只跑界面这一层, 不联网。
 * bridge 的 ABI 没有调用方上下文, 所以这里只保存自己拥有的句柄。 */
static struct {
    app_dash_handle_t controller;
} backend;

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    return app_dash_create(ui, &backend.controller);
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    (void)ui;
    app_dash_snapshot_t snapshot;
    if (app_dash_read(backend.controller, &snapshot) == ESP_GSP_OK) {
        fprintf(stderr,
                "dash_backend: tick=%u refresh=%u snapshot=%u command=%u page=%u last_error=%d\n",
                (unsigned)snapshot.tick, (unsigned)snapshot.refresh_count,
                (unsigned)snapshot.snapshot_count, (unsigned)snapshot.command_count,
                (unsigned)snapshot.last_page, (int)snapshot.last_error);
    }
    /* bridge 在 poll 退出后才调 deinit, 此时不会再有回调用这个状态。 */
    const esp_gsp_err_t err = app_dash_destroy(&backend.controller);
    if (err != ESP_GSP_OK) {
        fprintf(stderr, "dash_backend: cleanup failed (%d)\n", err);
    }
}
