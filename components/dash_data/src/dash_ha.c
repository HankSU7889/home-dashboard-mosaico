/**
 * @file dash_ha.c
 * 看板 —— Home Assistant 直连(esp_http_client + cJSON)
 */

#include "dash_ha.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <nvs_flash.h>

/* HA 地址/令牌来自运行时配置(dash_cfg), 回读间隔在这里定 */
#include "dash_cache.h"
#include "dash_cfg.h"
#include "dash_lock.h"


static const char * TAG = "dash_ha";

#define DASH_HA_TIMEOUT_MS 6000   /* 单次请求超时 */
#define DASH_HA_STATE_BUF  1536   /* 实体状态 JSON 的接收缓冲 */
#define DASH_HA_URL_MAX    80
#define DASH_HA_TOKEN_MAX  256
#define DASH_HA_AUTH_MAX   288
#define DASH_HA_POLL_INTERVAL_MS 5000   /* 设备状态回读间隔(毫秒) */

/* 排队等发出去的命令 */
typedef struct {
    char    entity_id[64];
    uint8_t domain;
    int32_t value;
} dash_ha_cmd_t;

/* 抄一份实体清单: 后台任务只认这份, 不用碰上层(避免跨线程抢) */
typedef struct {
    char    entity_id[64];
    uint8_t domain;
} dash_ha_ent_t;

static char s_base[DASH_HA_URL_MAX] = "";
static char s_token[DASH_HA_TOKEN_MAX] = "";
static char s_auth[DASH_HA_AUTH_MAX] = "";

static QueueHandle_t s_cmd_q = NULL;
static TaskHandle_t  s_task = NULL;
static volatile bool s_running = false;

static dash_ha_ent_t s_ent[DASH_DEVICE_MAX];
static uint8_t        s_ent_count = 0;

/* 状态快照: 后台任务写, 上层读 */
static int32_t s_state[DASH_DEVICE_MAX];
static bool    s_state_valid[DASH_DEVICE_MAX];
static bool    s_state_dirty = false;
static dash_lock_t s_state_lock;

/* 离线降级: HA 连不上时用 NVS 缓存顶着, 卡片文字变淡 */
static bool s_stale = false;

/* ===================== 命令发出后的"期望值" =====================
 *
 * HA 收到服务调用之后, 实体的状态不是立刻就变的(本地集成一般几十毫秒,
 * 走网关/云端的能到几百毫秒)。这段时间里 3 秒一次的回读会读到旧值,
 * 把用户刚拖出来的滑块位置又拽回去 —— 看着就像"没反应, 过一会又弹回来"。
 *
 * 所以命令发出后先记下期望值: 回读到不一样的值先压着不刷界面,
 * 等 HA 的状态跟期望一致了(或者等超时了)再认 HA 的。
 *
 * 这个数组只在后台任务里用(发命令和回读都在同一个任务), 不用加锁。
 */
#define DASH_HA_PENDING_MS  5000   /* 期望值的有效期(毫秒) */
#define DASH_HA_PENDING_TOL 2      /* 亮度换算有误差, 差这么多以内算一致 */

typedef struct {
    char    entity_id[64];
    int32_t value;      /* 期望的值 0~100 */
    int64_t ts_ms;      /* 命令发出的时刻 */
    bool    used;
} dash_ha_pending_t;

static dash_ha_pending_t s_pending[DASH_DEVICE_MAX];

/* 找这个实体有没有在等的期望值, 没有返回 -1 */
static int dash_ha_pending_find(const char * entity_id)
{
    if(entity_id == NULL) return -1;
    for(uint8_t i = 0; i < DASH_DEVICE_MAX; i++) {
        if(s_pending[i].used && strcmp(s_pending[i].entity_id, entity_id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* 记下期望值 */
static void dash_ha_pending_set(const char * entity_id, int32_t value)
{
    if(entity_id == NULL) return;

    int idx = dash_ha_pending_find(entity_id);
    if(idx < 0) {
        for(uint8_t i = 0; i < DASH_DEVICE_MAX; i++) {
            if(!s_pending[i].used) {
                idx = (int)i;
                break;
            }
        }
    }
    /* 满了就顶掉第一条 */
    if(idx < 0) idx = 0;

    snprintf(s_pending[idx].entity_id, sizeof(s_pending[idx].entity_id), "%s", entity_id);
    s_pending[idx].value = value;
    s_pending[idx].ts_ms = esp_timer_get_time() / 1000;
    s_pending[idx].used  = true;
}

/* 期望值作废 */
static void dash_ha_pending_clear(const char * entity_id)
{
    int idx = dash_ha_pending_find(entity_id);
    if(idx >= 0) s_pending[idx].used = false;
}

/* WiFi 事件标志(回调在 esp_event 任务里, 只置标志) */
static volatile bool s_net_down = false;
static volatile bool s_net_reconnect = false;
static bool s_events_registered = false;

/* 诊断计数 */
static int s_cmd_ok = 0;
static int s_cmd_err = 0;
static int s_poll_ok = 0;
static int s_poll_err = 0;
static char s_err[64] = {0};
static dash_lock_t s_stat_lock;

static void dash_ha_note_err(const char * fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    dash_lock_enter(&s_stat_lock);
    vsnprintf(s_err, sizeof(s_err), fmt, ap);
    dash_lock_leave(&s_stat_lock);
    va_end(ap);
    ESP_LOGW(TAG, "%s", s_err);
}

/* ===================== 配置 ===================== */

/* ---------------- WiFi 事件 ----------------
 *
 * IP_EVENT_STA_GOT_IP: 重连成功 -> 立刻全量回读一遍。
 * WIFI_EVENT_STA_DISCONNECTED: 断了 -> 标记离线(卡片变淡)。
 *
 * 回调跑在 esp_event 任务里, 只置标志, 不发 HTTP 也不碰 LVGL。
 */
static void dash_ha_ip_event_handler(void * arg, esp_event_base_t base, int32_t id, void * data)
{
    (void)arg;
    (void)data;
    if(base == IP_EVENT && (id == IP_EVENT_STA_GOT_IP || id == IP_EVENT_ETH_GOT_IP)) {
        ESP_LOGI(TAG, "拿到 IP, 立即回读一遍设备状态");
        s_net_down = false;
        s_net_reconnect = true;
    }
}

static void dash_ha_wifi_event_handler(void * arg, esp_event_base_t base, int32_t id, void * data)
{
    (void)arg;
    (void)data;
    if(base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi 断了");
        s_net_down = true;
    }
}

static void dash_ha_load_config(void)
{
    /* 地址和令牌都来自运行时配置(服务器下发 / NVS 缓存) */
    const dash_config_t * cfg = dash_cfg_get();
    strlcpy(s_base, cfg->ha_url, sizeof(s_base));
    strlcpy(s_token, cfg->ha_token, sizeof(s_token));

    /* 结尾多余的 '/' 去掉, 拼路径时少一个斜杠 */
    size_t bl = strlen(s_base);
    while(bl > 0 && s_base[bl - 1] == '/') {
        s_base[--bl] = 0;
    }

    snprintf(s_auth, sizeof(s_auth), "Bearer %s", s_token);
    /* 只打"配没配", 绝不打令牌本身 */
    ESP_LOGI(TAG, "HA: base=%s, 长令牌 %s", s_base[0] ? s_base : "(没配)",
             s_token[0] ? "已配置" : "没配");
}

void dash_ha_reload_config(void)
{
    dash_ha_load_config();
}

/* ===================== HTTP ===================== */

/* 建一个带鉴权头的 client。HA 一般是 http, 所以不挂证书包。 */
static esp_http_client_handle_t dash_ha_client_new(const char * url, esp_http_client_method_t method)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = DASH_HA_TIMEOUT_MS,
    };
    if(strncmp(url, "https", 5) == 0) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if(client == NULL) {
        dash_ha_note_err("http client 初始化失败");
        return NULL;
    }
    if(s_auth[0] != 0) {
        esp_http_client_set_header(client, "Authorization", s_auth);
    }
    return client;
}

/* GET, 响应进 buf。成功返回读到的字节数, 失败返回负数。 */
static int dash_ha_http_get(const char * url, char * buf, int buf_len)
{
    if(url == NULL || buf == NULL || buf_len <= 1) return -1;

    esp_http_client_handle_t client = dash_ha_client_new(url, HTTP_METHOD_GET);
    if(client == NULL) return -1;

    esp_http_client_set_header(client, "Accept", "application/json");

    int rc = -1;
    if(esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if(status == 200) {
            int total = 0;
            while(total < buf_len - 1) {
                int n = esp_http_client_read(client, buf + total, buf_len - 1 - total);
                if(n <= 0) break;
                total += n;
            }
            buf[total] = 0;
            rc = total;
        } else {
            dash_ha_note_err("GET %s 状态 %d", url, status);
        }
        esp_http_client_close(client);
    } else {
        dash_ha_note_err("GET %s 打不开", url);
    }
    esp_http_client_cleanup(client);
    return rc;
}

/* POST 一个 JSON body。成功返回 0。
 *
 * 这里必须用 esp_http_client_perform(), 不能手工 open()+fetch_headers()。
 * 原因: esp_http_client_open() 里有这么一行 "client->post_len = write_len",
 * 传 0 的话会把 set_post_field() 填好的长度清零:
 *   - Content-Length 变成 0
 *   - esp_http_client_send_post_data() 因为 post_len==0 直接跳过, 请求体压根没发
 * HA 收到一个空 POST, 回 400, 于是"拖了半天没反应, 灯也不动"。
 * perform() 内部会老老实实把请求体写出去。
 *
 * 另外 set_post_field() 只存指针不拷贝, body 必须在调用期间一直有效。
 */
static int dash_ha_http_post(const char * url, const char * body, const char * label)
{
    if(url == NULL || body == NULL) return -1;

    esp_http_client_handle_t client = dash_ha_client_new(url, HTTP_METHOD_POST);
    if(client == NULL) return -1;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    int rc = -1;
    esp_err_t perr = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    /* HA 调服务返回 200(实体状态数组) */
    if(status == 200 || status == 201) {
        rc = 0;
    } else {
        /* 状态码放前面: 报错串只有 64 字节, 后面跟 URL 会被截掉, 看不出到底哪一步失败 */
        dash_ha_note_err("POST %s 状态 %d", (label != NULL) ? label : "?", status);
        if(perr != ESP_OK) {
            ESP_LOGW(TAG, "perform 失败 %s", esp_err_to_name(perr));
        }
    }
    esp_http_client_cleanup(client);
    return rc;
}

/* ===================== 控制 ===================== */

int dash_ha_send_command(const char * entity_id, uint8_t domain, int32_t value)
{
    if(entity_id == NULL || entity_id[0] == 0 || s_base[0] == 0) {
        dash_ha_note_err("没配 HA 地址");
        return -1;
    }
    if(value < 0) value = 0;
    if(value > 100) value = 100;

    const char * dom = (domain == DASH_HA_FAN) ? "fan" : "light";
    char url[192];
    char body[160];
    char label[32];

    if(value <= 0) {
        /* 关: light/turn_off 或 fan/turn_off */
        snprintf(url, sizeof(url), "%s/api/services/%s/turn_off", s_base, dom);
        snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", entity_id);
        snprintf(label, sizeof(label), "%s/turn_off", dom);
    } else if(domain == DASH_HA_FAN) {
        /* 风扇: 用百分比 */
        snprintf(url, sizeof(url), "%s/api/services/fan/set_percentage", s_base);
        snprintf(body, sizeof(body), "{\"entity_id\":\"%s\",\"percentage\":%d}", entity_id, (int)value);
        snprintf(label, sizeof(label), "fan/set_pct");
    } else {
        /* 灯: 用亮度(HA 是 0~255) */
        snprintf(url, sizeof(url), "%s/api/services/light/turn_on", s_base);
        snprintf(body, sizeof(body), "{\"entity_id\":\"%s\",\"brightness\":%d}", entity_id,
                 (int)(value * 255 / 100));
        snprintf(label, sizeof(label), "%s/turn_on", dom);
    }

    ESP_LOGI(TAG, "控制 %s %s -> %d", dom, entity_id, (int)value);
    return dash_ha_http_post(url, body, label);
}

void dash_ha_request(const char * entity_id, uint8_t domain, int32_t value)
{
    if(s_cmd_q == NULL || entity_id == NULL) return;

    dash_ha_cmd_t cmd;
    snprintf(cmd.entity_id, sizeof(cmd.entity_id), "%s", entity_id);
    cmd.domain = domain;
    cmd.value  = value;
    /* 队列满了就丢最老的: 控制命令要的是"最新意图" */
    if(xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE) {
        dash_ha_cmd_t old;
        xQueueReceive(s_cmd_q, &old, 0);
        xQueueSend(s_cmd_q, &cmd, 0);
    }
}

/* ===================== 状态回读 ===================== */

int dash_ha_get_state(const char * entity_id, uint8_t domain, dash_ha_state_t * out)
{
    if(entity_id == NULL || entity_id[0] == 0 || out == NULL) return -1;

    memset(out, 0, sizeof(*out));

    char url[192];
    snprintf(url, sizeof(url), "%s/api/states/%s", s_base, entity_id);

    char buf[DASH_HA_STATE_BUF];
    int n = dash_ha_http_get(url, buf, sizeof(buf));
    if(n <= 0) {
        dash_ha_note_err("读状态失败 %s", entity_id);
        return -1;
    }

    cJSON * root = cJSON_Parse(buf);
    if(root == NULL) {
        dash_ha_note_err("状态 JSON 解析失败 %s", entity_id);
        return -1;
    }

    int rc = -1;
    cJSON * st    = cJSON_GetObjectItem(root, "state");
    cJSON * attrs = cJSON_GetObjectItem(root, "attributes");
    const char * state = cJSON_GetStringValue(st);
    if(state != NULL) {
        snprintf(out->state, sizeof(out->state), "%s", state);
        out->valid = true;

        if(strcmp(state, "unavailable") == 0 || strcmp(state, "unknown") == 0) {
            /* 实体不可用: 别去改界面, 保留原来的显示 */
            out->valid = false;
            rc = 0;
        } else {
            bool on = (strcmp(state, "on") == 0);
            out->on = on;
            if(!on) {
                out->value = 0;
            } else {
                /* 开着但读不到亮度/风速, 就当满档 */
                int32_t value = 100;
                if(domain == DASH_HA_FAN) {
                    cJSON * p = attrs ? cJSON_GetObjectItem(attrs, "percentage") : NULL;
                    if(cJSON_IsNumber(p)) value = p->valueint;
                } else {
                    cJSON * b = attrs ? cJSON_GetObjectItem(attrs, "brightness") : NULL;
                    if(cJSON_IsNumber(b)) value = (b->valueint * 100 + 127) / 255;
                }
                if(value < 0) value = 0;
                if(value > 100) value = 100;
                out->value = value;
            }
            rc = 0;
        }
    } else {
        dash_ha_note_err("状态里没有 state 字段 %s", entity_id);
    }

    cJSON_Delete(root);
    return rc;
}

/* ===================== 后台任务 ===================== */

static void dash_ha_task(void * arg)
{
    (void)arg;
    /* NVS 要读 flash, 在后台线程里读, 不占 LVGL 线程 */
    dash_ha_load_config();

    /* 等网络起来 */
    vTaskDelay(pdMS_TO_TICKS(3000));

    TickType_t last_poll = xTaskGetTickCount();
    const TickType_t iv_poll = pdMS_TO_TICKS(DASH_HA_POLL_INTERVAL_MS);

    while(s_running) {
        /* 1) 排队里的控制命令先发出去 */
        dash_ha_cmd_t cmd;
        while(xQueueReceive(s_cmd_q, &cmd, 0) == pdTRUE) {
            int rc = dash_ha_send_command(cmd.entity_id, cmd.domain, cmd.value);
            dash_lock_enter(&s_stat_lock);
            if(rc == 0) s_cmd_ok++; else s_cmd_err++;
            dash_lock_leave(&s_stat_lock);
            if(rc == 0) {
                /* 命令送到了: 记下期望值, HA 还没生效之前别用旧状态盖界面 */
                dash_ha_pending_set(cmd.entity_id, cmd.value);
            } else {
                /* 没送出去: 期望值作废, 让回读按实际情况刷 */
                dash_ha_pending_clear(cmd.entity_id);
            }
        }

        /* 2) 到点了回读一遍所有设备 */
        TickType_t now = xTaskGetTickCount();
        /* WiFi 重连成功 -> 不等间隔, 立刻回读一遍 */
        bool force_poll = false;
        if(s_net_reconnect) {
            s_net_reconnect = false;
            s_net_down = false;
            force_poll = true;
        }

        if(force_poll || (now - last_poll >= iv_poll)) {
            last_poll = now;

            int32_t vals[DASH_DEVICE_MAX];
            bool    vld[DASH_DEVICE_MAX];
            memset(vals, 0, sizeof(vals));
            memset(vld, 0, sizeof(vld));

            bool any_ok = false;
            for(uint8_t i = 0; i < s_ent_count; i++) {
                dash_ha_state_t st;
                int rc = dash_ha_get_state(s_ent[i].entity_id, s_ent[i].domain, &st);
                dash_lock_enter(&s_stat_lock);
                if(rc == 0) s_poll_ok++; else s_poll_err++;
                dash_lock_leave(&s_stat_lock);
                if(rc == 0 && st.valid) {
                    any_ok = true;

                    /* 有在等的期望值? 对不上就先别刷界面(留着用户拖出来的值) */
                    int pidx = dash_ha_pending_find(s_ent[i].entity_id);
                    if(pidx >= 0) {
                        int32_t diff = st.value - s_pending[pidx].value;
                        if(diff < 0) diff = -diff;
                        if(diff <= DASH_HA_PENDING_TOL) {
                            /* HA 跟上了: 期望值作废 */
                            s_pending[pidx].used = false;
                        } else if((esp_timer_get_time() / 1000) - s_pending[pidx].ts_ms < DASH_HA_PENDING_MS) {
                            /* HA 还没跟上: 这次回读不覆盖界面 */
                            continue;
                        } else {
                            /* 超时了: 还是认 HA 的 */
                            s_pending[pidx].used = false;
                        }
                    }

                    vals[i] = st.value;
                    vld[i]  = true;
                }
            }

            /* 一台都没读到 -> 说明 HA 连不上了, 拿 NVS 缓存顶上 */
            if(any_ok) {
                /* 读到了就存一份, 下次断网时用 */
                dash_ha_item_t items[DASH_DEVICE_MAX];
                int n = 0;
                for(uint8_t i = 0; i < s_ent_count; i++) {
                    if(!vld[i]) continue;
                    /* 显式按长度拷: snprintf 的截断分析看不出 s_ent[i] 的
                     * 单条上限(它按整个数组算), 会误报"可能截断"。 */
                    size_t id_len = strnlen(s_ent[i].entity_id, sizeof(items[n].entity_id));
                    memcpy(items[n].entity_id, s_ent[i].entity_id, id_len);
                    items[n].entity_id[id_len] = 0;
                    items[n].value = vals[i];
                    items[n].on    = vals[i] > 0;
                    items[n].valid = true;
                    n++;
                }
                if(n > 0) {
                    dash_cache_save_ha(items, n);
                }
                dash_lock_enter(&s_state_lock);
                s_stale = false;
                dash_lock_leave(&s_state_lock);
            } else {
                /* 全军覆没: 读 NVS 缓存, 按 entity_id 对上号 */
                dash_ha_item_t items[DASH_DEVICE_MAX];
                int n = 0;
                if(dash_cache_load_ha(items, DASH_DEVICE_MAX, &n) && n > 0) {
                    memset(vals, 0, sizeof(vals));
                    memset(vld, 0, sizeof(vld));
                    for(uint8_t i = 0; i < s_ent_count; i++) {
                        for(int k = 0; k < n; k++) {
                            if(strcmp(items[k].entity_id, s_ent[i].entity_id) == 0) {
                                vals[i] = items[k].value;
                                vld[i]  = items[k].valid;
                                break;
                            }
                        }
                    }
                    ESP_LOGW(TAG, "HA 连不上, 用 NVS 缓存顶上");
                }
                dash_lock_enter(&s_state_lock);
                s_stale = true;
                dash_lock_leave(&s_state_lock);
            }

            dash_lock_enter(&s_state_lock);
            memcpy(s_state, vals, sizeof(vals));
            memcpy(s_state_valid, vld, sizeof(vld));
            s_state_dirty = true;
            dash_lock_leave(&s_state_lock);
        }

        vTaskDelay(pdMS_TO_TICKS(150));
    }

    ESP_LOGI(TAG, "后台任务退出");
    s_task = NULL;
    vTaskDelete(NULL);
}

void dash_ha_start(const dash_ha_device_t * devices, int count)
{
    if(s_running) return;
    if(devices == NULL || count <= 0) {
        ESP_LOGW(TAG, "没有设备清单, HA 后台任务不启动");
        return;
    }

    /* 状态锁: 后台任务写, 上层和诊断线程读(跨核, 必须用互斥量) */
    dash_lock_init(&s_state_lock);
    dash_lock_init(&s_stat_lock);

    /* 监听 WiFi: 断了标记离线, 重连了立刻回读一遍 */
    if(s_events_registered == false) {
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, dash_ha_ip_event_handler, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, dash_ha_ip_event_handler, NULL);
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, dash_ha_wifi_event_handler, NULL);
        s_events_registered = true;
    }

    /* 抄一份实体清单: 后台任务只碰这份, 调用方销毁也不怕 */
    s_ent_count = 0;
    for(int i = 0; i < count && i < DASH_DEVICE_MAX; i++) {
        const dash_ha_device_t * dev = &devices[i];
        if(dev->entity_id[0] == 0) continue;
        snprintf(s_ent[s_ent_count].entity_id, sizeof(s_ent[s_ent_count].entity_id), "%s", dev->entity_id);
        s_ent[s_ent_count].domain = dev->domain;
        s_ent_count++;
    }

    if(s_cmd_q == NULL) {
        s_cmd_q = xQueueCreate(4, sizeof(dash_ha_cmd_t));
    }
    if(s_cmd_q == NULL) {
        ESP_LOGE(TAG, "命令队列创建失败");
        return;
    }

    s_running = true;
    TaskHandle_t task = NULL;
    /* 栈给够: HTTP + cJSON + 状态缓冲 */
    xTaskCreate(dash_ha_task, "dash_ha", 12288, NULL, 5, &task);
    s_task = task;
    if(s_task == NULL) {
        s_running = false;
        ESP_LOGE(TAG, "起任务失败");
        return;
    }

    ESP_LOGI(TAG, "HA 后台任务已启动(%d 个设备)", s_ent_count);
}

void dash_ha_stop(void)
{
    s_running = false;

    if(s_events_registered) {
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, dash_ha_ip_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, dash_ha_ip_event_handler);
        esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, dash_ha_wifi_event_handler);
        s_events_registered = false;
    }

    /* 等后台任务自己退出(它最多卡在一个 HTTP 请求上, 6 秒超时) */
    for(int i = 0; i < 80 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_task = NULL;

    if(s_cmd_q != NULL) {
        vQueueDelete(s_cmd_q);
        s_cmd_q = NULL;
    }
    s_ent_count = 0;
}

void dash_ha_get_status(dash_ha_status_t * out)
{
    if(out == NULL) return;
    memset(out, 0, sizeof(*out));
    dash_lock_enter(&s_stat_lock);
    out->cmd_ok   = s_cmd_ok;
    out->cmd_err  = s_cmd_err;
    out->poll_ok  = s_poll_ok;
    out->poll_err = s_poll_err;
    snprintf(out->err, sizeof(out->err), "%s", s_err);
    dash_lock_leave(&s_stat_lock);

    dash_lock_enter(&s_state_lock);
    out->stale = s_stale;
    dash_lock_leave(&s_state_lock);
    /* HA 连不上 = 回读全军覆没, 或者 WiFi 直接断了 */
    out->offline = out->stale || s_net_down;
}

int dash_ha_snapshot(dash_ha_state_t * out, int max_count)
{
    if(out == NULL || max_count <= 0) return 0;

    int32_t vals[DASH_DEVICE_MAX];
    bool    vld[DASH_DEVICE_MAX];

    dash_lock_enter(&s_state_lock);
    memcpy(vals, s_state, sizeof(vals));
    memcpy(vld, s_state_valid, sizeof(vld));
    dash_lock_leave(&s_state_lock);

    int n = 0;
    for(uint8_t i = 0; i < s_ent_count && i < DASH_DEVICE_MAX && n < max_count; i++) {
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].state, sizeof(out[n].state), "%s", vld[i] ? "on" : "off");
        out[n].valid = vld[i];
        out[n].on    = vld[i] && vals[i] > 0;
        out[n].value = vld[i] ? vals[i] : 0;
        n++;
    }
    return n;
}
