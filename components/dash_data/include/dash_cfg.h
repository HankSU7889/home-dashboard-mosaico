/**
 * @file dash_cfg.h
 * 看板数据层 —— 运行时配置(服务器下发)
 *
 * 设计原则: 敏感信息不进固件。
 *   HA 长令牌、天气 API Key、股票代码、服务器地址这些都由看板服务器在运行时下发,
 *   板子拿到后写进 NVS; 之后每次开机先读 NVS 顶着, 联网后再刷新一份。
 *
 * 配置从哪来(优先级从高到低):
 *   1) 看板服务器 GET {server_base}/device/config?device_id=<身份>
 *   2) NVS(dashcfg 命名空间)里的上一次结果
 *   3) 编译期默认值(只放不含机密的占位值)
 *
 * "设备身份"决定服务器下发哪一套配置 —— 同一份固件既能装在客厅那块板,
 * 也能装在卧室那块板, 不用重新编译。这就是多环境配置切换的实现方式。
 */
#ifndef DASH_CFG_H_
#define DASH_CFG_H_

#include <stdbool.h>
#include <stdint.h>

#include "dash_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** NVS 命名空间。 */
#define DASH_CFG_NVS_NS "dashcfg"

/** 编译期兜底值(不含任何机密的占位配置)。 */
#ifndef DASH_CFG_DEFAULT_TITLE
#define DASH_CFG_DEFAULT_TITLE "家庭看板"
#endif

typedef struct {
    char    server_base[128];      /**< 看板服务器地址, 形如 http://<服务器IP>:8766 */
    char    device_id[32];         /**< 本机身份, 服务器据此选择配置套 */
    char    title[24];             /**< 界面标题, 如 "客厅" */
    char    ha_url[96];            /**< Home Assistant 地址 */
    char    ha_token[256];         /**< Home Assistant 长令牌 */
    char    qweather_key[80];      /**< 和风天气 Key */
    char    qweather_host[64];     /**< 和风天气域名 */
    char    qweather_location[32]; /**< 城市 ID, 如 101010100 */
    char    stock_codes[128];      /**< 股票代码, 逗号分隔 */
    char    stock_names[128];      /**< 股票显示名, 与代码一一对应 */
    int     device_count;          /**< 设备清单条数, 0..DASH_DEVICE_MAX */
    dash_ha_device_t devices[DASH_DEVICE_MAX];
} dash_config_t;

/**
 * 初始化配置层: 读 NVS 缓存, 再尝试从 Vibe Mode 共享信息里取服务器地址。
 * 必须在任何 dash_cfg_get() 之前调一次。返回 ESP_OK 表示拿到了可用的服务器地址。
 */
int dash_cfg_init(void);

/**
 * 从服务器拉一份配置下来。
 * @param out 收到的配置(可选, NULL 表示只要内部生效)
 * @return ESP_OK 表示拉到了并已生效
 */
int dash_cfg_fetch(dash_config_t *out);

/** 把一份配置写进运行期并落盘到 NVS。 */
int dash_cfg_apply(const dash_config_t *cfg);

/** 取当前生效的配置(返回的是内部副本, 调用方不要改)。 */
const dash_config_t *dash_cfg_get(void);

/** 取当前配置的服务器地址(空串表示还没配好)。 */
const char *dash_cfg_server_base(void);

/** 取当前配置的设备身份。 */
const char *dash_cfg_device_id(void);

#ifdef __cplusplus
}
#endif

#endif  // DASH_CFG_H_
