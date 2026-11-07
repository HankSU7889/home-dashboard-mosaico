/**
 * @file dash_sysstat.h
 * 看板数据层 —— 服务器系统状态(PVE 主机 / 磁盘 / 网络负载)
 */
#ifndef DASH_SYSSTAT_H_
#define DASH_SYSSTAT_H_

#include "dash_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 设看板服务器地址(形如 http://<服务器地址>:8766)。 */
void dash_sysstat_set_server_base(const char *base);

/**
 * 拉一份服务器系统状态。
 * 内部会顺便测一下板子到服务器的往返延迟(填到 rtt_ms)。
 * @return 成功返回 0, 失败返回负数
 */
int dash_fetch_sysstat(dash_sysstat_t *out);

#ifdef __cplusplus
}
#endif

#endif  // DASH_SYSSTAT_H_
