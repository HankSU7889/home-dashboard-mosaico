/**
 * @file dash_stock.h
 * 看板数据层 —— 股票行情(腾讯行情 qt.gtimg.cn)
 */
#ifndef DASH_STOCK_H_
#define DASH_STOCK_H_

#include "dash_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 拉一批股票的行情。
 * @param out       输出数组
 * @param max_count 最多几只
 * @return 实际取到的条数, 失败返回负数
 */
int dash_fetch_stocks(dash_quote_t *out, int max_count);

#ifdef __cplusplus
}
#endif

#endif  // DASH_STOCK_H_
