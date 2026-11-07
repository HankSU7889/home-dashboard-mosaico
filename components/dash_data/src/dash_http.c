/**
 * @file dash_http.c
 * 看板数据层 —— 通用 HTTP GET、gzip 解压与轻量 JSON 工具
 *
 * 和风天气无条件返回 gzip(连 Accept-Encoding: identity 都不理),
 * 而 IDF 的 esp_http_client 没有内置解压, 压缩字节直接喂 cJSON 必定解析失败。
 * 这里用 ROM 自带的 miniz(tinfl) 解压 —— ROM 里就有, 不额外占 flash。
 */

#include "dash_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <miniz.h>

static const char *TAG = "dash_http";

/* 关机时置位, 让正在重试的 HTTP 立刻放弃, 不用干等超时 */
static volatile bool s_abort = false;

/* 最近一次 HTTP 失败的原因(诊断用) */
static char s_http_err[64] = {0};
/* 最近一次 HTTP 的原始字节数和解压后的字节数(诊断用) */
static int s_http_raw = 0;
static int s_http_plain = 0;
static long s_http_clen = -1;
/* 最近一次响应是不是被截断了(收到的字节数比 content-length 少) */
static bool s_http_trunc = false;
/* 累计下载字节数, 用来算板子的下载速率(只在数据层任务里改, 无竞争) */
static int32_t s_bytes_down = 0;

void dash_http_set_abort(bool abort)
{
    s_abort = abort;
}

const char *dash_http_last_error(void)
{
    return s_http_err;
}

void dash_http_last_size(int *out_raw, int *out_plain)
{
    if (out_raw) {
        *out_raw = s_http_raw;
    }
    if (out_plain) {
        *out_plain = s_http_plain;
    }
}

/* 累计下载字节数(诊断用) */
int32_t dash_http_bytes_down(void)
{
    return s_bytes_down;
}

/* ===================== gzip 解压 =====================
 *
 * 和风天气无条件返回 gzip(连 Accept-Encoding: identity 都不理),
 * 而 IDF 的 esp_http_client 没有内置解压, 压缩字节直接喂 cJSON 必定解析失败。
 * 这里用 ESP32-S3 ROM 自带的 miniz(tinfl) 解压 —— ROM 里就有, 不额外占 flash。
 */

/* 跳过 gzip 头, 找出 DEFLATE 数据起点。
 * gzip 头 = 10 字节固定(魔数/CM/FLG/MTIME/XFL/OS) + FEXTRA/FNAME/FCOMMENT/FHCRC 可选段 */
static bool gzip_deflate_offset(const uint8_t * in, int in_len, int * off)
{
    if(in == NULL || off == NULL || in_len < 18) {
        return false;
    }
    if(in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) {
        return false;
    }

    uint8_t flg = in[3];
    int p = 10;

    if(flg & 0x04) { /* FEXTRA */
        if(p + 2 > in_len) {
            return false;
        }
        int xlen = in[p] | (in[p + 1] << 8);
        p += 2 + xlen;
    }
    if(flg & 0x08) { /* FNAME */
        while(p < in_len && in[p] != 0) {
            p++;
        }
        p++;
    }
    if(flg & 0x10) { /* FCOMMENT */
        while(p < in_len && in[p] != 0) {
            p++;
        }
        p++;
    }
    if(flg & 0x02) { /* FHCRC */
        p += 2;
    }
    if(p >= in_len) {
        return false;
    }

    *off = p;
    return true;
}

/* gzip -> 明文。成功返回明文字节数, 失败返回负数 */
static int gzip_inflate(const uint8_t * in, int in_len, uint8_t * out, int out_len)
{
    int off = 0;
    if(!gzip_deflate_offset(in, in_len, &off)) {
        ESP_LOGW(TAG, "gzip 头解析失败");
        return -1;
    }

    /* tinfl 状态机约 11KB, 放堆上(优先 PSRAM), 别压任务栈 */
    static tinfl_decompressor * s_decomp = NULL;
    if(s_decomp == NULL) {
        s_decomp = (tinfl_decompressor *)heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if(s_decomp == NULL) {
            s_decomp = (tinfl_decompressor *)heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_8BIT);
        }
        if(s_decomp == NULL) {
            ESP_LOGE(TAG, "解压状态机分配失败");
            return -1;
        }
    }
    tinfl_init(s_decomp);

    size_t in_left = (size_t)(in_len - off);
    size_t out_left = (size_t)out_len;
    tinfl_status st = tinfl_decompress(s_decomp, in + off, &in_left,
                                       out, out, &out_left,
                                       TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    int produced = (int)((size_t)out_len - out_left);
    if(st != TINFL_STATUS_DONE) {
        /* 尾部被截断(输入不够 或 输出缓冲满了)时, 已经解出来的那截照样能用 ——
         * 和风的 "now" / "daily" 都在响应最前面, 调用方只抠这几个对象,
         * 尾巴上的 refer/station 丢了无所谓。 */
        if(produced > 0) {
            ESP_LOGW(TAG, "gzip 解压没解完(状态=%d), 先用已解出的 %d 字节", (int)st, produced);
            return produced;
        }
        ESP_LOGW(TAG, "gzip 解压失败, 状态=%d", (int)st);
        return -1;
    }
    return produced;
}

/* ===================== 通用 HTTP GET ===================== */

int dash_http_get(const char * url, char * buf, int buf_len, int timeout_ms, int retry)
{
    if(url == NULL || buf == NULL || buf_len <= 1) {
        return -1;
    }

    for(int attempt = 0; attempt <= retry; attempt++) {
        if(s_abort) {
            ESP_LOGW(TAG, "收到中止信号, 放弃请求");
            return -1;
        }
        esp_http_client_config_t config = {
            .url = url,
            .method = HTTP_METHOD_GET,
            .timeout_ms = timeout_ms,
            /* 用 IDF 自带的 CA 证书包做 HTTPS 证书校验 */
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if(client == NULL) {
            ESP_LOGE(TAG, "http client 初始化失败");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        esp_err_t err = esp_http_client_open(client, 0);
        if(err != ESP_OK) {
            snprintf(s_http_err, sizeof(s_http_err), "打开失败 %s", esp_err_to_name(err));
            ESP_LOGW(TAG, "打开 %s 失败: %s", url, esp_err_to_name(err));
            esp_http_client_cleanup(client);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* 先把响应头读完, 之后才能拿到状态码 */
        int64_t content_len = esp_http_client_fetch_headers(client);
        s_http_clen = (long)content_len;
        int status = esp_http_client_get_status_code(client);
        if(status != 200) {
            snprintf(s_http_err, sizeof(s_http_err), "状态 %d", status);
            ESP_LOGW(TAG, "%s 返回状态 %d", url, status);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int total = 0;
        int failed = 0;
        while(total < buf_len - 1) {
            int len = esp_http_client_read(client, buf + total, buf_len - 1 - total);
            if(len < 0) {
                snprintf(s_http_err, sizeof(s_http_err), "读取出错");
                ESP_LOGW(TAG, "%s 读取出错", url);
                failed = 1;
                break;
            }
            if(len == 0) {
                break;  /* 读完了 */
            }
            total += len;
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if(!failed && total > 0) {
            /* 看魔数判断是不是 gzip。
             * 不去读 Content-Encoding 响应头: IDF 的 esp_http_client_get_header
             * 拿到的是请求头, 读不到响应头, 靠魔数反而更稳(服务端压了就一定带这俩字节)。 */
            s_http_raw = total;
            s_http_plain = total;   /* 没压缩的话明文就是原样, 诊断里好对比 */
            if(total >= 18 && (uint8_t)buf[0] == 0x1f && (uint8_t)buf[1] == 0x8b) {
                uint8_t * plain = (uint8_t *)heap_caps_malloc(buf_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if(plain == NULL) {
                    plain = (uint8_t *)heap_caps_malloc(buf_len, MALLOC_CAP_8BIT);
                }
                if(plain == NULL) {
                    ESP_LOGE(TAG, "解压缓冲分配失败");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    continue;
                }
                int raw = gzip_inflate((const uint8_t *)buf, total, plain, buf_len - 1);
                if(raw > 0) {
                    memcpy(buf, plain, raw);
                    total = raw;
                    s_http_plain = raw;
                }
                heap_caps_free(plain);
                if(raw <= 0) {
                    ESP_LOGW(TAG, "%s gzip 解压失败", url);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    continue;
                }
            }
            buf[total] = 0;
            s_bytes_down += total;
            s_http_err[0] = 0;

            /* 诊断: 收到的字节数 vs 服务端声明的 content-length。
             * 不一致说明响应被截断了(缓冲太小/网络断流), 日志里一眼能看出来。 */
            s_http_trunc = false;
            if(content_len > 0 && s_http_raw > 0 && s_http_raw < content_len) {
                s_http_trunc = true;
                ESP_LOGW(TAG, "响应被截断: 收到 %d 字节, content-length=%lld",
                        s_http_raw, (long long)content_len);
            }
            /* 明文正好把缓冲塞满 => 解压输出被截断(后面的字段拿不到) */
            if(s_http_plain > 0 && s_http_plain >= buf_len - 1) {
                s_http_trunc = true;
                ESP_LOGW(TAG, "解压后明文塞满缓冲(%d 字节), 尾部可能丢了", s_http_plain);
            }
            if(s_http_trunc) {
                /* 截断也要留个话给诊断上报, 远程一眼能看出数据为什么不全 */
                snprintf(s_http_err, sizeof(s_http_err), "响应截断 raw=%d plain=%d cl=%ld",
                         s_http_raw, s_http_plain, s_http_clen);
            }

            ESP_LOGI(TAG, "GET %s -> %d 字节 (raw=%d plain=%d content-length=%lld%s)",
                     url, total, s_http_raw, s_http_plain, (long long)content_len,
                     s_http_trunc ? " [截断]" : "");
            return total;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if(s_http_err[0] == 0) {
        snprintf(s_http_err, sizeof(s_http_err), "重试 %d 次仍失败", retry);
    }
    ESP_LOGE(TAG, "GET %s 重试 %d 次仍失败: %s", url, retry, s_http_err);
    return -1;
}


static const char *dash_json_obj_end(const char * p)
{
    if(p == NULL || *p != '{') {
        return NULL;
    }
    int depth = 0;
    bool in_str = false;
    for(const char * q = p; *q != '\0'; q++) {
        if(in_str) {
            if(*q == '\\') {
                q++;      /* 跳过转义的那个字符 */
                continue;
            }
            if(*q == '"') {
                in_str = false;
            }
            continue;
        }
        if(*q == '"') {
            in_str = true;
            continue;
        }
        if(*q == '{') {
            depth++;
        } else if(*q == '}') {
            depth--;
            if(depth == 0) {
                return q;
            }
        }
    }
    return NULL;   /* 没闭合: 尾部被截断了 */
}

/* 从一段 JSON 里抠出 "key":{...} 这个对象的原文。
 *
 * 为什么不用整包 cJSON_Parse: 和风的响应尾巴上挂着一大堆 refer/station 数据,
 * 整包一大而且慢, 万一尾部被截断整包就都废了 ——
 * 而我们要的温度/天气状况都在最前面的 "now" 对象里, 单独抠出来解析最稳。
 * 返回值指向对象起始的 '{', 并把长度写进 *out_len; 找不到/没闭合返回 NULL。
 */
const char *dash_json_pick_object(const char * json, const char * key, int * out_len)
{
    if(json == NULL || key == NULL || out_len == NULL) {
        return NULL;
    }
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char * p = strstr(json, pat);
    if(p == NULL) {
        return NULL;
    }
    p = strchr(p + strlen(pat), '{');
    if(p == NULL) {
        return NULL;
    }
    const char * e = dash_json_obj_end(p);
    if(e == NULL) {
        return NULL;
    }
    *out_len = (int)(e - p) + 1;
    return p;
}

/* 取 "key":"value" 里的字符串(和风的 code 字段就这么写)。找不到返回 -1。 */
int dash_json_pick_string(const char * json, const char * key, char * out, int out_len)
{
    if(json == NULL || key == NULL || out == NULL || out_len <= 0) {
        return -1;
    }
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char * p = strstr(json, pat);
    if(p == NULL) {
        return -1;
    }
    p = strchr(p + strlen(pat), '"');
    if(p == NULL) {
        return -1;
    }
    p++;    /* 跳过起始引号 */
    int i = 0;
    while(*p != '\0' && *p != '"' && i < out_len - 1) {
        out[i++] = *p++;
    }
    out[i] = 0;
    return 0;
}

/* 抠出 "key":[ {...}, {...} ] 数组里每个对象的原文。
 * 数组尾巴被截断时, 后面抠不出来的对象自动丢掉, 已经抠出来的照样能用。
 * 返回抠到的个数。 */
int dash_json_pick_array_objects(const char * json, const char * key,
                                   const char ** items, int * lens, int max_items)
{
    if(json == NULL || key == NULL || items == NULL || lens == NULL || max_items <= 0) {
        return 0;
    }
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char * p = strstr(json, pat);
    if(p == NULL) {
        return 0;
    }
    p = strchr(p + strlen(pat), '[');
    if(p == NULL) {
        return 0;
    }
    p++;    /* 跳过 '[' */
    int n = 0;
    while(n < max_items) {
        const char * o = strchr(p, '{');
        if(o == NULL) {
            break;
        }
        const char * e = dash_json_obj_end(o);
        if(e == NULL) {
            break;      /* 这个对象没闭合: 尾巴被截断了, 后面的不要了 */
        }
        items[n] = o;
        lens[n] = (int)(e - o) + 1;
        n++;
        p = e + 1;
    }
    return n;
}

/* 取 JSON 里的字符串字段, 拷贝到定长缓冲 */
/**
 * 取一个"可能是字符串也可能是数字"的字段。
 * 和风有些字段类型不统一: 比如 aqi 有时是数字 32, 有时是字符串 "32",
 * 只判一种类型会漏数据。
 */
int dash_json_val(cJSON * obj, const char * key, char * out, int out_len)
{
    cJSON * item = cJSON_GetObjectItem(obj, key);
    if(item == NULL) {
        return -1;
    }
    if(cJSON_IsNumber(item)) {
        snprintf(out, out_len, "%d", (int)cJSON_GetNumberValue(item));
        return 0;
    }
    const char * val = cJSON_GetStringValue(item);
    if(val == NULL) {
        return -1;
    }
    snprintf(out, out_len, "%s", val);
    return 0;
}
