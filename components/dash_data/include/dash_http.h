/**
 * @file dash_http.h
 * 看板数据层 —— 通用 HTTP GET 与轻量 JSON 工具
 *
 * 自己实现解压和"抠对象"解析, 是因为和风天气无条件返回 gzip,
 * 而 esp_http_client 没有内置解压; 而且它们的响应尾巴上挂着一大坨
 * refer/station 数据, 整包 cJSON_Parse 遇到一处截断就整包都废。
 */
#ifndef DASH_HTTP_H_
#define DASH_HTTP_H_

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 单次请求超时(毫秒)。 */
#define DASH_HTTP_TIMEOUT_MS 8000
/** 失败重试次数(总请求数 = 重试 + 1)。 */
#define DASH_HTTP_MAX_RETRY  2
/** 接收缓冲大小。 */
#define DASH_HTTP_BODY_MAX   4096

/**
 * 通用 HTTP GET。
 * @param url        完整 URL
 * @param buf        接收缓冲(会被补一个结尾 0)
 * @param buf_len    缓冲大小
 * @param timeout_ms 单次请求超时(毫秒)
 * @param retry      失败重试次数
 * @return 读到的字节数(不含结尾 0), 失败返回负数
 */
int dash_http_get(const char *url, char *buf, int buf_len, int timeout_ms, int retry);

/** 置位后, 正在重试的 HTTP 会立刻放弃, 不用干等超时。 */
void dash_http_set_abort(bool abort);

/** 取最近一次 HTTP 失败的原因(诊断用, 不含敏感信息)。 */
const char *dash_http_last_error(void);

/** 取最近一次响应体解压前后的字节数(诊断用)。 */
void dash_http_last_size(int *out_raw, int *out_plain);

/** 累计下载字节数(诊断用, 用来算板子的下载速率)。 */
int32_t dash_http_bytes_down(void);

/* ---------------- 轻量 JSON 工具 ---------------- */

/**
 * 从一段 JSON 里抠出 "key":{...} 这个对象的原文。
 * 返回值指向对象起始的 '{', 并把长度写进 *out_len; 找不到/没闭合返回 NULL。
 */
const char *dash_json_pick_object(const char *json, const char *key, int *out_len);

/** 取 "key":"value" 里的字符串。找不到返回 -1。 */
int dash_json_pick_string(const char *json, const char *key, char *out, int out_len);

/**
 * 抠出 "key":[ {...}, {...} ] 数组里每个对象的原文。
 * 数组尾巴被截断时, 后面抠不出来的对象自动丢掉, 已经抠出来的照样能用。
 * 返回抠到的个数。
 */
int dash_json_pick_array_objects(const char *json, const char *key,
                                  const char **items, int *lens, int max_items);

/**
 * 取一个"可能是字符串也可能是数字"的字段。
 * 和风有些字段类型不统一: 比如 aqi 有时是数字 32, 有时是字符串 "32"。
 */
int dash_json_val(cJSON *obj, const char *key, char *out, int out_len);

#ifdef __cplusplus
}
#endif

#endif  // DASH_HTTP_H_
