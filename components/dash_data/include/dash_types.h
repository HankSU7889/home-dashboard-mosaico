/**
 * @file dash_types.h
 * 看板数据层 —— 共享的数据结构(不依赖任何 UI 框架)
 *
 * 这一层是 7C(LVGL 版)和 Mosaico(GSP 版)共用的"词汇表":
 * 天气、股票、服务器状态、HA 设备状态都用这里的结构描述,
 * 数据层负责填, 上层负责显示, 两者之间只靠这些结构体和快照接口通信。
 */
#ifndef DASH_TYPES_H_
#define DASH_TYPES_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/** 最多跟踪几只股票。 */
#define DASH_STOCK_MAX  4
/** 最多跟踪几台 Home Assistant 设备。 */
#define DASH_DEVICE_MAX 5
/** 最多跟踪几台服务器主机。 */
#define DASH_HOST_MAX   3

/* ---------------- 天气 ---------------- */

typedef struct {
    char temp[8];         /**< 当前温度, 如 "26" */
    char desc[24];        /**< 天气状况, 如 "多云" */
    char humidity[8];     /**< 相对湿度 %, 如 "48" */
    char high[16];        /**< 今日最高温 */
    char low[16];         /**< 今日最低温 */
    char sunrise[8];      /**< 日出, 如 "06:28" */
    char sunset[8];       /**< 日落, 如 "17:46" */
    char aqi[8];          /**< 空气质量数值, 如 "36" */
    char aqi_level[12];   /**< 空气质量等级, 如 "优" */
    char forecast[3][48]; /**< 未来三天, 如 "明天 31/24" */
} dash_weather_data_t;

/* ---------------- 股票 ---------------- */

typedef struct {
    char name[16];    /**< 名称 */
    char price[16];   /**< 当前价 */
    char pct[12];     /**< 涨跌幅, 如 "-1.47%" */
    int8_t direction; /**< >0 涨 <0 跌 0 平 */
} dash_quote_t;

/* ---------------- 服务器系统状态 ---------------- */

typedef struct {
    char  name[16];      /**< 主机名 */
    char  ip[24];        /**< 内网地址 */
    float cpu_pct;       /**< CPU 占用 % */
    float mem_pct;       /**< 内存占用 % */
    float steal_pct;     /**< steal % */
    bool  online;        /**< 这台是不是探到了 */
} dash_host_stat_t;

typedef struct {
    dash_host_stat_t hosts[DASH_HOST_MAX];
    int   host_count;      /**< 实际有几台 */
    float steal_pct;       /**< steal 均值 */
    float load;            /**< 服务器 1 分钟负载 */
    int   mem_pct;         /**< 服务器内存占用 % */
    int   disk_pct;        /**< 服务器磁盘占用 % */
    int   disk_free_gb;    /**< 磁盘剩余 GB */
    int   rtt_ms;          /**< 板子到服务器的往返延迟 */
    int   down_kbps;       /**< 板子下载速率 KB/s */
    bool  valid;           /**< 这份数据是不是真的拿到了 */
} dash_sysstat_t;

/* ---------------- Home Assistant ---------------- */

/** HA 的设备域。 */
#define DASH_HA_LIGHT 0
#define DASH_HA_FAN   1

typedef struct {
    int32_t value;     /**< 0~100(灯光=亮度百分比, 风扇=风速百分比) */
    bool    on;        /**< 设备是不是开着 */
    bool    valid;     /**< 实体是不是可用(unavailable 时为 false, 别改界面) */
    char    state[16]; /**< HA 原始状态: on / off / unavailable */
} dash_ha_state_t;

/** 设备清单里的一项。 */
typedef struct {
    char    name[24];      /**< 显示名, 如 "客厅灯带" */
    char    entity_id[64]; /**< HA 实体 ID */
    uint8_t domain;        /**< DASH_HA_LIGHT / DASH_HA_FAN */
} dash_ha_device_t;


/* ---------------- 数据层快照 ---------------- */

/**
 * 数据层的一份完整快照。
 *
 * 放在 dash_types.h 是因为它要同时被数据层和界面层看到,
 * 而界面层(以及 PC 模拟器)不该为了看数据而依赖 ESP-IDF。
 */
typedef struct {
    dash_weather_data_t weather;
    bool weather_valid;      /**< 天气是不是真的拿到过 */
    bool weather_stale;      /**< 天气是不是用的是缓存(界面要变淡) */

    dash_quote_t stocks[DASH_STOCK_MAX];
    int  stock_count;
    bool stock_stale;

    dash_sysstat_t sysstat;
    bool sysstat_valid;
    bool sysstat_stale;

    dash_ha_state_t devices[DASH_DEVICE_MAX];
    int  device_count;
    bool device_stale;

    bool offline;            /**< 现在是不是拿不到实时数据 */
    char title[24];          /**< 标题(来自服务器下发的配置) */
    char err[80];            /**< 最近一次错误 */
} dash_snapshot_t;

#ifdef __cplusplus
}
#endif

#endif  // DASH_TYPES_H_
