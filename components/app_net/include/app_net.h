/**
 * @file app_net.h
 * 看板 —— WiFi 接入
 *
 * Mosaico 的配网流程跟 7C 不一样: 不再用板子自己起的 SoftAP,
 * 而是在 Vibe Mode 里填好 WiFi, 凭据存进 sysmeta/shared 的保留字段。
 * 应用开机时用 mosaico_shared_get() 把凭据读出来连网 ——
 * 固件里不存任何密码, 也不需要重新编译。
 *
 * 板子上的用法:
 *   Vibe Mode -> 设置 -> 共享信息 -> wifi_ssid / wifi_password
 *   Vibe Mode -> 设置 -> 共享信息 -> dash_server(看板服务器地址)
 */
#ifndef APP_NET_H_
#define APP_NET_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 netif / 事件循环 / WiFi, 并按共享信息里的凭据连网。 */
esp_err_t app_net_start(void);

/** 停 WiFi(不常用, 主要是测试用)。 */
esp_err_t app_net_stop(void);

/** 当前是否已经拿到 IP。 */
bool app_net_is_connected(void);

/** 取当前 IP(没连上就返回空串)。 */
const char *app_net_ip(void);

/** 取当前 SSID(没连上就返回空串; 不打印密码)。 */
const char * app_net_ssid(void);

#ifdef __cplusplus
}
#endif

#endif  // APP_NET_H_
