// SPDX-License-Identifier: Apache-2.0
/**
 * @file dash_data_stub.c
 * 模拟器用的假数据层
 *
 * 模拟器跑在 PC 上: 没有 WiFi、没有 NVS、没有 HTTP, 真数据层根本编不过也跑不了。
 * 这里用一份"看起来像真的"的静态数据顶上, 目的只有一个 —— 让版面能在模拟器里
 * 被完整地看见: 每个字段都有内容、行数是满的、陈旧态遮罩也能被触发。
 *
 * 真机上这一层换回 components/dash_data, 界面代码一行都不用改。
 */

#include <stdio.h>
#include <string.h>

#include "dash_cfg.h"
#include "dash_data.h"

/* ---------------- 静态假数据 ---------------- */

/* 设备清单: 名字 + HA 实体 ID(模拟器里不会真的去连) */
static const dash_ha_device_t STUB_DEVICES[DASH_DEVICE_MAX] = {
    { "客厅灯带", "light.study_light",    DASH_HA_LIGHT },
    { "主卧吊灯", "light.bedroom_ceiling", DASH_HA_LIGHT },
    { "玄关射灯", "light.hall_spotlight",  DASH_HA_LIGHT },
    { "书房空调", "fan.study_ac",         DASH_HA_FAN   },
    { "阳台风扇", "fan.balcony_fan",      DASH_HA_FAN   },
};

/* 假配置: 界面层只关心设备清单和标题 */
static dash_config_t s_stub_cfg;

static void stub_cfg_init(void)
{
    memset(&s_stub_cfg, 0, sizeof(s_stub_cfg));
    snprintf(s_stub_cfg.title, sizeof(s_stub_cfg.title), "%s", "客厅");
    snprintf(s_stub_cfg.device_id, sizeof(s_stub_cfg.device_id), "%s", "simulator");
    s_stub_cfg.device_count = DASH_DEVICE_MAX;
    for (int i = 0; i < DASH_DEVICE_MAX; i++) {
        s_stub_cfg.devices[i] = STUB_DEVICES[i];
    }
}

/* 造一份快照: 天气/股票/服务器状态/设备全是"有值"的 */
static void stub_fill_snapshot(dash_snapshot_t *snap)
{
    memset(snap, 0, sizeof(*snap));

    dash_weather_data_t *w = &snap->weather;
    snprintf(w->temp, sizeof(w->temp), "%s", "26");
    snprintf(w->desc, sizeof(w->desc), "%s", "多云");
    snprintf(w->humidity, sizeof(w->humidity), "%s", "48");
    snprintf(w->high, sizeof(w->high), "%s", "29");
    snprintf(w->low, sizeof(w->low), "%s", "21");
    snprintf(w->sunrise, sizeof(w->sunrise), "%s", "06:28");
    snprintf(w->sunset, sizeof(w->sunset), "%s", "17:46");
    snprintf(w->aqi, sizeof(w->aqi), "%s", "36");
    snprintf(w->aqi_level, sizeof(w->aqi_level), "%s", "优");
    snprintf(w->forecast[0], sizeof(w->forecast[0]), "%s", "今天 29/21");
    snprintf(w->forecast[1], sizeof(w->forecast[1]), "%s", "明天 31/24");
    snprintf(w->forecast[2], sizeof(w->forecast[2]), "%s", "后天 33/25");
    snap->weather_valid = true;

    static const char *STOCK_NAME[DASH_STOCK_MAX]  = { "贵州茅台", "平安银行", "宁德时代", "中国平安" };
    static const char *STOCK_PRICE[DASH_STOCK_MAX] = { "1682.50", "11.83", "238.16", "52.07" };
    static const char *STOCK_PCT[DASH_STOCK_MAX]   = { "+0.86%", "-1.24%", "+2.15%", "+0.42%" };
    static const int8_t  STOCK_DIR[DASH_STOCK_MAX]  = { 1, -1, 1, 1 };
    for (int i = 0; i < DASH_STOCK_MAX; i++) {
        snprintf(snap->stocks[i].name, sizeof(snap->stocks[i].name), "%s", STOCK_NAME[i]);
        snprintf(snap->stocks[i].price, sizeof(snap->stocks[i].price), "%s", STOCK_PRICE[i]);
        snprintf(snap->stocks[i].pct, sizeof(snap->stocks[i].pct), "%s", STOCK_PCT[i]);
        snap->stocks[i].direction = STOCK_DIR[i];
    }
    snap->stock_count = DASH_STOCK_MAX;

    dash_sysstat_t *st = &snap->sysstat;
    static const char *HOST_NAME[DASH_HOST_MAX] = { "客厅 NAS", "PVE 主机", "软路由" };
    /* 用 RFC 5737 的文档保留段(192.0.2.0/24), 一眼就知道是假数据 */
    static const char *HOST_IP[DASH_HOST_MAX]   = { "192.0.2.2", "192.0.2.3", "192.0.2.1" };
    static const float HOST_CPU[DASH_HOST_MAX] = { 12.5f, 38.0f, 7.5f };
    static const float HOST_MEM[DASH_HOST_MAX] = { 46.0f, 71.0f, 22.0f };
    for (int i = 0; i < DASH_HOST_MAX; i++) {
        snprintf(st->hosts[i].name, sizeof(st->hosts[i].name), "%s", HOST_NAME[i]);
        snprintf(st->hosts[i].ip, sizeof(st->hosts[i].ip), "%s", HOST_IP[i]);
        st->hosts[i].cpu_pct = HOST_CPU[i];
        st->hosts[i].mem_pct = HOST_MEM[i];
        st->hosts[i].online = true;
    }
    st->host_count = DASH_HOST_MAX;
    st->load = 1.42f;
    st->mem_pct = 58;
    st->disk_pct = 63;
    st->disk_free_gb = 118;
    st->rtt_ms = 12;
    st->down_kbps = 348;
    st->valid = true;
    snap->sysstat_valid = true;

    for (int i = 0; i < DASH_DEVICE_MAX; i++) {
        snap->devices[i].value = (i == 1) ? 0 : (i * 20 + 30);
        snap->devices[i].on = snap->devices[i].value > 0;
        snap->devices[i].valid = true;
        snprintf(snap->devices[i].state, sizeof(snap->devices[i].state), "%s",
                 snap->devices[i].on ? "on" : "off");
    }
    snap->device_count = DASH_DEVICE_MAX;

    snprintf(snap->title, sizeof(snap->title), "%s", s_stub_cfg.title);
}

/* ---------------- 对外的假接口 ---------------- */

static dash_data_notify_fn s_notify = NULL;
static void *s_notify_ctx = NULL;
static dash_snapshot_t s_snapshot;

int dash_data_start(dash_data_notify_fn notify, void *ctx)
{
    stub_cfg_init();
    s_notify = notify;
    s_notify_ctx = ctx;
    stub_fill_snapshot(&s_snapshot);

    /* 真机上第一份快照是联网之后才到的; 模拟器没有网, 立刻推一份,
     * 这样界面一初始化就是满的, 不用干等。 */
    if (s_notify != NULL) {
        s_notify(&s_snapshot, s_notify_ctx);
    }
    return 0;
}

int dash_data_stop(void)
{
    s_notify = NULL;
    s_notify_ctx = NULL;
    return 0;
}

bool dash_data_get_snapshot(dash_snapshot_t *out)
{
    if (out == NULL) {
        return false;
    }
    *out = s_snapshot;
    return true;
}

int dash_data_set_device(int index, int32_t value)
{
    if (index < 0 || index >= DASH_DEVICE_MAX) {
        return -1;
    }
    if (value < 0) {
        value = 0;
    }
    if (value > 100) {
        value = 100;
    }
    s_snapshot.devices[index].value = value;
    s_snapshot.devices[index].on = value > 0;
    s_snapshot.devices[index].valid = true;
    snprintf(s_snapshot.devices[index].state, sizeof(s_snapshot.devices[index].state), "%s",
             value > 0 ? "on" : "off");
    if (s_notify != NULL) {
        s_notify(&s_snapshot, s_notify_ctx);
    }
    return 0;
}

int dash_data_refresh(void)
{
    if (s_notify != NULL) {
        s_notify(&s_snapshot, s_notify_ctx);
    }
    return 0;
}

void dash_data_get_diag(dash_data_diag_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->weather_ok = 1;
    out->stock_ok = 1;
    out->sysstat_ok = 1;
    out->config_ok = 1;
}

/* 界面层要拿设备清单来画那五行 */
const dash_config_t *dash_cfg_get(void)
{
    return &s_stub_cfg;
}

const char *dash_cfg_server_base(void)
{
    return "";
}

const char *dash_cfg_device_id(void)
{
    return s_stub_cfg.device_id;
}
