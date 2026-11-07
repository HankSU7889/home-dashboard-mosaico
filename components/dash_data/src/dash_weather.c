/**
 * @file dash_weather.c
 * 看板数据层 —— 天气(和风天气 QWeather)
 *
 * 三路接口各自独立成败: 实时(now) / 三日预报(3d) / 空气质量(air)。
 * 谁成功了谁更新自己的那几个字段, 失败的那一路不会把别的路刚拿到的实时数据冲掉。
 *
 * 域名、城市、API Key 全部来自运行时配置(dash_cfg), 不写死在固件里。
 */

#include "dash_weather.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>
#include <esp_log.h>

#include "dash_cfg.h"
#include "dash_http.h"

static const char *TAG = "dash_weather";

/* 最近一次错误(天气走三路接口, 错误要能说清是哪一路) */
static char s_err[80] = {0};
/* 空气质量专用的错误原因(空气的间隔最长, 单独留一份免得被别的请求冲掉) */
static char s_air_err[80] = {0};

static int qweather_get(const char * path, char * buf, int buf_len)
{
    const dash_config_t * cfg = dash_cfg_get();
    char url[384];
    snprintf(url, sizeof(url), "https://%s/v7/weather/%s?location=%s&key=%s",
             cfg->qweather_host, path, cfg->qweather_location, cfg->qweather_key);
    return dash_http_get(url, buf, buf_len, DASH_HTTP_TIMEOUT_MS, DASH_HTTP_MAX_RETRY);
}

int dash_fetch_weather_now(dash_weather_data_t * out)
{
    if(out == NULL) {
        return -1;
    }
    if(dash_cfg_get()->qweather_key[0] == 0) {
        ESP_LOGW(TAG, "没配 QWeather key, 跳过天气");
        return -1;
    }

    char * buf = (char *)malloc(DASH_HTTP_BODY_MAX);
    if(buf == NULL) {
        return -1;
    }

    int ret = -1;
    int n = qweather_get("now", buf, DASH_HTTP_BODY_MAX);
    if(n > 0) {
        char code[16] = {0};
        dash_json_pick_string(buf, "code", code, sizeof(code));

        /* 只抠 "now" 出来解析。
         * 和风的响应是 {"code":..,"now":{..},"refer":{..},"station":{..}},
         * 尾巴上挂着 refer/station 一大坨, 整包只要有一处被截断就整包都废;
         * 而温度和天气状况都在最前面的 "now" 里, 单独抠出来最稳。 */
        int obj_len = 0;
        const char * obj = dash_json_pick_object(buf, "now", &obj_len);
        if(obj == NULL) {
            int raw = 0, plain = 0;
            dash_http_last_size(&raw, &plain);
            ESP_LOGE(TAG, "天气响应里找不到 now 对象 n=%d raw=%d plain=%d", n, raw, plain);
            snprintf(s_err, sizeof(s_err), "天气now=%d raw=%d plain=%d", n, raw, plain);
        } else {
            char objbuf[512];
            if(obj_len > (int)sizeof(objbuf) - 1) {
                obj_len = (int)sizeof(objbuf) - 1;
            }
            memcpy(objbuf, obj, obj_len);
            objbuf[obj_len] = 0;

            cJSON * now = cJSON_Parse(objbuf);
            if(now != NULL) {
                if(code[0] == 0 || strcmp(code, "200") == 0) {
                    dash_json_val(now, "temp", out->temp, sizeof(out->temp));
                    dash_json_val(now, "text", out->desc, sizeof(out->desc));
                    dash_json_val(now, "humidity", out->humidity, sizeof(out->humidity));
                    ESP_LOGI(TAG, "天气: %s %s°C", out->desc, out->temp);
                    ret = 0;
                } else {
                    ESP_LOGW(TAG, "天气接口返回异常 code=%s", code);
                    snprintf(s_err, sizeof(s_err), "天气code=%s", code);
                }
                cJSON_Delete(now);
            } else {
                int raw = 0, plain = 0;
                dash_http_last_size(&raw, &plain);
                ESP_LOGE(TAG, "天气 now 解析失败 n=%d raw=%d plain=%d obj_len=%d",
                         n, raw, plain, obj_len);
                snprintf(s_err, sizeof(s_err), "天气now失败n=%d raw=%d plain=%d",
                         n, raw, plain);
            }
        }
    }

    free(buf);
    return ret;
}

int dash_fetch_weather_forecast(dash_weather_data_t * out)
{
    if(out == NULL) {
        return -1;
    }
    if(dash_cfg_get()->qweather_key[0] == 0) {
        return -1;
    }

    char * buf = (char *)malloc(DASH_HTTP_BODY_MAX);
    if(buf == NULL) {
        return -1;
    }

    int ret = -1;
    int n = qweather_get("3d", buf, DASH_HTTP_BODY_MAX);
    if(n > 0) {
        /* "daily":[ {...}, {...}, {...} ] —— 逐个抠出来解析,
         * 这样就算最后一个对象被截断, 前两天的数据照样能显示。 */
        const char * items[3];
        int lens[3];
        static const char * day_name[3] = {"今天", "明天", "后天"};

        int cnt = dash_json_pick_array_objects(buf, "daily", items, lens, 3);
        int idx = 0;
        for(int i = 0; i < cnt; i++) {
            char objbuf[512];
            int len = lens[i];
            if(len > (int)sizeof(objbuf) - 1) {
                len = (int)sizeof(objbuf) - 1;
            }
            memcpy(objbuf, items[i], len);
            objbuf[len] = 0;

            cJSON * item = cJSON_Parse(objbuf);
            if(item == NULL) {
                ESP_LOGW(TAG, "预报第 %d 条解析失败, 跳过", i + 1);
                continue;
            }
            char tmax[8] = {0};
            char tmin[8] = {0};
            char text[24] = {0};
            dash_json_val(item, "tempMax", tmax, sizeof(tmax));
            dash_json_val(item, "tempMin", tmin, sizeof(tmin));
            dash_json_val(item, "textDay", text, sizeof(text));

            /* 天气图标没有字形, 只显示 描述+最高/最低 */
            snprintf(out->forecast[idx], sizeof(out->forecast[idx]), "%s %s/%s",
                     text[0] ? text : day_name[idx], tmax, tmin);

            if(idx == 0) {
                /* 第一条就是今天: 顺便拿日出日落和今日最高/最低温 */
                dash_json_val(item, "sunrise", out->sunrise, sizeof(out->sunrise));
                dash_json_val(item, "sunset", out->sunset, sizeof(out->sunset));
                snprintf(out->high, sizeof(out->high), "%s", tmax);
                snprintf(out->low, sizeof(out->low), "%s", tmin);
            }
            idx++;
            cJSON_Delete(item);
        }
        if(idx > 0) {
            ret = 0;
            ESP_LOGI(TAG, "预报 %d 天: %s / %s / %s",
                     idx, out->forecast[0], out->forecast[1], out->forecast[2]);
        } else {
            int raw = 0, plain = 0;
            dash_http_last_size(&raw, &plain);
            ESP_LOGE(TAG, "预报里没抠到 daily n=%d raw=%d plain=%d", n, raw, plain);
            snprintf(s_err, sizeof(s_err), "预报daily=0 raw=%d plain=%d", raw, plain);
        }
    }

    free(buf);
    return ret;
}

int dash_fetch_weather_air(dash_weather_data_t * out)
{
    if(out == NULL) {
        return -1;
    }
    if(dash_cfg_get()->qweather_key[0] == 0) {
        return -1;
    }

    char url[384];
    /* 空气质量也是 v7 接口, 参数同样是 location= */
    const dash_config_t * cfg = dash_cfg_get();
    snprintf(url, sizeof(url), "https://%s/v7/air/now?location=%s&key=%s",
             cfg->qweather_host, cfg->qweather_location, cfg->qweather_key);

    char * buf = (char *)malloc(DASH_HTTP_BODY_MAX);
    if(buf == NULL) {
        return -1;
    }

    int ret = -1;
    int n = dash_http_get(url, buf, DASH_HTTP_BODY_MAX, DASH_HTTP_TIMEOUT_MS, DASH_HTTP_MAX_RETRY);
    if(n > 0) {
        /* 只抠 "now" 出来解析(后面的监测站数组又大又用不上) */
        int obj_len = 0;
        const char * obj = dash_json_pick_object(buf, "now", &obj_len);
        if(obj == NULL) {
            int raw = 0, plain = 0;
            dash_http_last_size(&raw, &plain);
            ESP_LOGE(TAG, "空气响应里找不到 now 对象 n=%d raw=%d plain=%d", n, raw, plain);
            snprintf(s_err, sizeof(s_err), "空气n=%d raw=%d plain=%d", n, raw, plain);
        } else {
            char objbuf[512];
            if(obj_len > (int)sizeof(objbuf)) {
                obj_len = (int)sizeof(objbuf);
            }
            memcpy(objbuf, obj, obj_len);
            objbuf[obj_len] = 0;
            cJSON * now = cJSON_Parse(objbuf);
            if(now != NULL) {
                /* aqi 可能是数字也可能是字符串, 用 json_val 两种都吃 */
                char aqi[8] = {0};
                if(dash_json_val(now, "aqi", aqi, sizeof(aqi)) == 0) {
                    snprintf(out->aqi, sizeof(out->aqi), "%s", aqi);
                    dash_json_val(now, "category", out->aqi_level, sizeof(out->aqi_level));
                    ESP_LOGI(TAG, "空气: %s %s", out->aqi_level, out->aqi);
                    ret = 0;
                } else {
                    snprintf(s_err, sizeof(s_err), "空气接口没有 aqi 字段");
                }
                cJSON_Delete(now);
            } else {
                int raw = 0, plain = 0;
                dash_http_last_size(&raw, &plain);
                ESP_LOGE(TAG, "空气 now 解析失败 n=%d raw=%d plain=%d obj_len=%d",
                         n, raw, plain, obj_len);
                snprintf(s_err, sizeof(s_err), "空气now失败n=%d raw=%d plain=%d", n, raw, plain);
            }
        }
    }

    free(buf);
    if(ret != 0) {
        snprintf(s_air_err, sizeof(s_air_err), "%s", s_err);
    } else {
        s_air_err[0] = 0;
    }
    return ret;
}
const char *dash_weather_last_error(void)
{
    return s_err;
}

const char *dash_weather_air_error(void)
{
    return s_air_err;
}
