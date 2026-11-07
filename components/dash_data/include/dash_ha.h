/**
 * @file dash_ha.h
 * 看板数据层 —— Home Assistant 直连
 *
 * 板子自己跟 HA 说话, 不经过本地中转服务器:
 *   控制: POST /api/services/<domain>/<action>   (带长令牌 Authorization 头)
 *   回读: GET  /api/states/<entity_id>
 *
 * 网络请求跑在后台任务里, 上层通过 dash_ha_snapshot() 取状态快照,
 * 不需要知道 HTTP 的存在 —— 这也是它和 UI 层解耦的关键。
 *
 * HA 地址和长令牌来自运行时配置(dash_cfg), 不写死在固件里。
 */
#ifndef DASH_HA_H_
#define DASH_HA_H_

#include <stdbool.h>
#include <stdint.h>

#include "dash_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 起后台任务: 每隔几秒回读一遍所有设备。
 * @param devices 设备清单(内部会抄一份, 之后调用方销毁也不影响任务)
 * @param count   设备条数
 */
void dash_ha_start(const dash_ha_device_t *devices, int count);

/** 停后台任务。 */
void dash_ha_stop(void);

/** 配置变更后重新读一次 HA 地址和令牌。 */
void dash_ha_reload_config(void);

/**
 * 发一条控制命令(阻塞, 几百毫秒, 只能在后台任务里调)。
 * @param entity_id HA 实体 ID
 * @param domain    DASH_HA_LIGHT / DASH_HA_FAN
 * @param value     0~100, 0 = 关闭
 * @return 成功返回 0, 失败返回负数
 */
int dash_ha_send_command(const char *entity_id, uint8_t domain, int32_t value);

/**
 * 把一条控制命令排进队列(上层线程里调, 不阻塞)。
 * 松手回调走这个: UI 线程一秒都不能卡在 HTTP 上。
 */
void dash_ha_request(const char *entity_id, uint8_t domain, int32_t value);

/**
 * 读一个实体的当前状态(阻塞, 只能在后台任务里调)。
 * @return 成功返回 0, 失败返回负数
 */
int dash_ha_get_state(const char *entity_id, uint8_t domain, dash_ha_state_t *out);

/**
 * 取所有设备的当前状态快照。
 * @param out       输出数组
 * @param max_count 最多几个
 * @return 有效(读到过状态)的设备条数
 */
int dash_ha_snapshot(dash_ha_state_t *out, int max_count);

/** ---------------- 诊断 ---------------- */

typedef struct {
    int  cmd_ok;    /**< 控制命令成功次数 */
    int  cmd_err;   /**< 控制命令失败次数 */
    int  poll_ok;   /**< 状态回读成功次数 */
    int  poll_err;  /**< 状态回读失败次数 */
    bool stale;     /**< 现在显示的是 NVS 缓存里的旧状态(HA 连不上) */
    bool offline;   /**< HA 连不上 */
    char err[64];   /**< 最近一次错误 */
} dash_ha_status_t;

/** 取一份状态快照。任何任务都能调。 */
void dash_ha_get_status(dash_ha_status_t *out);

#ifdef __cplusplus
}
#endif

#endif  // DASH_HA_H_
