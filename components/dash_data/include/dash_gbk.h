/**
 * @file dash_gbk.h
 * GBK -> UTF-8 转换(给腾讯股票接口的中文名用)
 */
#ifndef DASH_GBK_H_
#define DASH_GBK_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * GBK 字符串转 UTF-8。
 * @param gbk   输入(以 0 结尾)
 * @param out   输出缓冲
 * @param out_len 输出缓冲大小
 * @return 写入的字节数(不含结尾 0), 失败返回 -1
 */
int dash_gbk_to_utf8(const uint8_t * gbk, char * out, int out_len);

#ifdef __cplusplus
}
#endif

#endif  // DASH_GBK_H_
