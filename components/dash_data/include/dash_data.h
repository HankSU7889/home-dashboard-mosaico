/**
 * @file dash_data.h
 * 看板数据层 —— 统一入口(快照 + 通知)
 *
 * 上层(UI)只跟这个接口打交道:
 *   1) dash_data_start(cb, ctx) 起数据层, 之后每次有新数据就回调 cb
 *   2) 回调里拿到的是一份完整快照(dash_snapshot_t), 自己决定怎么显示
 *   3) dash_data_set_device() 发设备控制命令
 *
 * 数据层内部有三条线并行跑:
 *   - 天气/股票/服务器状态: 同一个后台任务按各自的间隔拉
 *   - Home Assistant: 独立的后台任务, 因为控制命令要能立刻插队
 *   - 配置下发: 开机和每次 WiFi 重连后都去服务器要一份最新配置
 */
#ifndef DASH_DATA_H_
#define DASH_DATA_H_

#include <stdbool.h>
#include <stdint.h>

#include "dash_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 数据层诊断信息(远程排查用)。 */
typedef struct {
    int  weather_ok;
    int  weather_err;
    int  stock_ok;
    int  stock_err;
    int  sysstat_ok;
    int  sysstat_err;
    int  config_ok;
    int  config_err;
    bool offline;
    char err[80];
} dash_data_diag_t;

/** 数据有变化时的通知回调。跑在数据层任务上下文里, 必须不阻塞。 */
typedef void (*dash_data_notify_fn)(const dash_snapshot_t *snapshot, void *ctx);

/**
 * 启动数据层。
 * @param notify 有新数据时的回调(NULL 表示不要通知)
 * @param ctx    回调上下文
 * @return 0 表示成功, 负数表示失败(取值为 ESP_ERR_*)
 */
int dash_data_start(dash_data_notify_fn notify, void *ctx);

/** 停止数据层(停后台任务)。 */
int dash_data_stop(void);

/** 取当前快照(任何任务都能调, 内部有锁)。 */
bool dash_data_get_snapshot(dash_snapshot_t *out);

/**
 * 控制一台设备(亮度/风速)。
 * @param index 设备序号, 0..DASH_DEVICE_MAX-1
 * @param value 0~100, 0 = 关闭
 */
int dash_data_set_device(int index, int32_t value);

/** 手动触发一次刷新(界面上的刷新按钮用)。 */
int dash_data_refresh(void);

/** 取一份状态诊断信息。 */
void dash_data_get_diag(dash_data_diag_t *out);

#ifdef __cplusplus
}
#endif

#endif  // DASH_DATA_H_
