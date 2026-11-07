/**
 * @file dash_weather.h
 * 看板数据层 —— 天气(和风天气 QWeather)
 */
#ifndef DASH_WEATHER_H_
#define DASH_WEATHER_H_

#include "dash_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 实时天气: 取当前温度和天气状况。成功返回 0。 */
int dash_fetch_weather_now(dash_weather_data_t *out);

/** 三天预报: 取最高/最低温、日出日落、未来三天。成功返回 0。 */
int dash_fetch_weather_forecast(dash_weather_data_t *out);

/** 空气质量(单独一个接口, 失败不影响其它天气数据)。成功返回 0。 */
int dash_fetch_weather_air(dash_weather_data_t *out);

/** 最近一次天气错误(诊断用)。 */
const char *dash_weather_last_error(void);

/** 最近一次空气质量错误(诊断用)。 */
const char *dash_weather_air_error(void);

#ifdef __cplusplus
}
#endif

#endif  // DASH_WEATHER_H_
