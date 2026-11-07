// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_gsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 看板 UI 控制器。
 *
 * 分工:
 *   场景 JSON(components/app_dash/ui/main.json) —— 版面、配色、静态文字、控件结构
 *   本文件                                        —— 数据怎么进界面、用户怎么操作
 *
 * 数据来源是 dash_data 组件(天气/股票/服务器状态/Home Assistant),
 * 它每拿到一份新快照就回调这里, 这里把快照翻译成 GSP 的 setter 调用。
 * setter 是往渲染任务队列里投命令, 所以在数据层任务上下文里调也安全。
 */
typedef struct app_dash *app_dash_handle_t;

/** 诊断快照: 给测试和远程排查用。 */
typedef struct {
    uint32_t tick;              /**< 定时器已跑的次数。 */
    uint32_t refresh_count;     /**< 手动刷新次数。 */
    uint32_t snapshot_count;    /**< 收到数据快照的次数。 */
    uint32_t command_count;     /**< 发出的设备控制命令数。 */
    uint32_t last_page;         /**< 当前页码, 0 起。 */
    esp_gsp_err_t last_error;   /**< 最近一次错误码。 */
} app_dash_snapshot_t;

/** 创建看板控制器; ui/out_handle 不能为 NULL, ui 必须活得比控制器久。 */
esp_gsp_err_t app_dash_create(esp_gsp_handle_t ui, app_dash_handle_t *out_handle);

/** 取诊断信息; 只能在 UI 循环里或派发停止后调。 */
esp_gsp_err_t app_dash_read(app_dash_handle_t handle, app_dash_snapshot_t *out_snapshot);

/** 摘掉回调、释放控制器并清空 *handle。 */
esp_gsp_err_t app_dash_destroy(app_dash_handle_t *handle);

#ifdef __cplusplus
}
#endif
