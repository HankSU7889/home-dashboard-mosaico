/**
 * @file dash_cache.h
 * 看板数据层 —— NVS 缓存(断网时拿上次的数据顶上)
 *
 * 每成功拉一次数据就往 NVS 里存一份, 下次拉不到(断网/服务器抽风)就把缓存读出来显示,
 * 界面上标成"陈旧"(文字变淡)。
 *
 * 注意: 缓存只是"兜底", 显示的是上一次成功的数据, 不是实时的。
 */
#ifndef DASH_CACHE_H_
#define DASH_CACHE_H_

#include <stdbool.h>
#include <stdint.h>

#include "dash_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** HA 设备状态(缓存用)。 */
typedef struct {
    char    entity_id[64];
    int32_t value;   /**< 0~100 */
    bool    on;      /**< 设备是不是开着 */
    bool    valid;   /**< 上次读到的是不是有效状态 */
} dash_ha_item_t;

/* 天气 */
void dash_cache_save_weather(const dash_weather_data_t *w);
bool dash_cache_load_weather(dash_weather_data_t *w);

/* 股票 */
void dash_cache_save_stocks(const dash_quote_t *stocks, int count);
bool dash_cache_load_stocks(dash_quote_t *stocks, int max_count, int *count);

/* 服务器系统状态 */
void dash_cache_save_sysstat(const dash_sysstat_t *st);
bool dash_cache_load_sysstat(dash_sysstat_t *st);

/* HA 设备状态 */
void dash_cache_save_ha(const dash_ha_item_t *items, int count);
bool dash_cache_load_ha(dash_ha_item_t *items, int max_count, int *count);

#ifdef __cplusplus
}
#endif

#endif  // DASH_CACHE_H_
