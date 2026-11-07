/**
 * @file dash_stock.c
 * 看板数据层 —— 股票行情(腾讯行情)
 *
 * 接口是 GBK 编码的纯文本, 字段用 ~ 分隔:
 *   v_sh600000="1~名称~600000~当前价~昨收~...~涨跌额~涨跌幅~...";
 * 显示名优先用配置里的(腾讯返回的是全称), 对不上才退回腾讯的名字。
 */

#include "dash_stock.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_log.h>

#include "dash_cfg.h"
#include "dash_http.h"
#include "dash_gbk.h"

static const char *TAG = "dash_stock";

/* 代码 -> 显示名(按配置顺序一一对应) */
static char s_code_list[DASH_STOCK_MAX][16];
static char s_name_list[DASH_STOCK_MAX][16];
static int  s_code_count = 0;

/* 把 "a,b,c" 拆成数组 */
static void dash_split_list(const char *src, char out[][16], int max_n, int *out_n)
{
    int n = 0;
    const char *p = src;
    while (p != NULL && *p != 0 && n < max_n) {
        const char *comma = strchr(p, ',');
        size_t len = (comma != NULL) ? (size_t)(comma - p) : strlen(p);
        if (len > 0 && len < 16) {
            memcpy(out[n], p, len);
            out[n][len] = 0;
            n++;
        }
        if (comma == NULL) {
            break;
        }
        p = comma + 1;
    }
    *out_n = n;
}

/* 按代码找显示名; 找不到返回 NULL */
static const char *dash_stock_display_name(const char *code)
{
    if (code == NULL || code[0] == 0) {
        return NULL;
    }
    for (int i = 0; i < s_code_count; i++) {
        if (strcmp(s_code_list[i], code) == 0) {
            return s_name_list[i];
        }
    }
    return NULL;
}

/* 从当前配置里刷新代码/显示名清单 */
static void stock_lists_refresh(void)
{
    const dash_config_t *cfg = dash_cfg_get();
    dash_split_list(cfg->stock_codes, s_code_list, DASH_STOCK_MAX, &s_code_count);

    char names[DASH_STOCK_MAX][16];
    int n_names = 0;
    dash_split_list(cfg->stock_names, names, DASH_STOCK_MAX, &n_names);
    for (int i = 0; i < s_code_count; i++) {
        if (i < n_names) {
            /* 显式按长度拷: snprintf 的截断分析看不出 names[i] 的单元素上限,
             * 会误报"可能截断"(其实两个数组元素一样宽, 拷不丢)。 */
            size_t len = strnlen(names[i], sizeof(s_name_list[i]));
            memcpy(s_name_list[i], names[i], len);
            s_name_list[i][len] = 0;
        } else {
            s_name_list[i][0] = 0;   /* 对不上就退回腾讯返回的名字 */
        }
    }
}

/* 从 "v_usAAPL=\"...\"" 里抠出 usAAPL(去掉 v_ 前缀) */
static void dash_quote_var_code(const char * start, const char * eq, char * out, size_t out_sz)
{
    const char * s = eq;
    while(s > start && (isalnum((unsigned char)s[-1]) || s[-1] == '_')) {
        s--;
    }
    size_t n = (size_t)(eq - s);
    if(n > 2 && s[0] == 'v' && s[1] == '_') {
        s += 2;
        n -= 2;
    }
    if(n >= out_sz) n = out_sz - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

int dash_fetch_stocks(dash_quote_t * out, int max_count)
{
    if(out == NULL || max_count <= 0) {
        return -1;
    }
    const dash_config_t * cfg = dash_cfg_get();
    if(cfg->stock_codes[0] == 0) {
        return -1;
    }

    char url[256];
    snprintf(url, sizeof(url), "https://qt.gtimg.cn/q=%s", cfg->stock_codes);
    stock_lists_refresh();

    char * buf = (char *)malloc(DASH_HTTP_BODY_MAX);
    if(buf == NULL) {
        return -1;
    }

    int count = 0;
    int n = dash_http_get(url, buf, DASH_HTTP_BODY_MAX, DASH_HTTP_TIMEOUT_MS, DASH_HTTP_MAX_RETRY);
    if(n > 0) {
        /* 返回长这样: v_sh600000="1~某只股票~600000~10.00~...~-0.20~-0.13~..."; */
        char * p = buf;
        while(p != NULL && *p != 0 && count < max_count) {
            char * eq = strstr(p, "=\"");
            if(eq == NULL) {
                break;
            }
            char * v = eq + 2;
            char * end = strchr(v, '"');
            if(end == NULL) {
                break;
            }

            /* 变量名: v_usAAPL -> usAAPL (拿来对上配置好的显示名) */
            char code[24];
            dash_quote_var_code(p, eq, code, sizeof(code));

            /* 按 ~ 切字段 */
            char * field[64];
            int nf = 0;
            char * cur = v;
            while(nf < 64) {
                field[nf++] = cur;
                char * sep = strchr(cur, '~');
                if(sep == NULL) {
                    break;
                }
                *sep = 0;
                cur = sep + 1;
            }

            /* 字段下标: [1]名称 [3]当前价 [4]昨收 [31]涨跌额 [32]涨跌幅% */
            if(nf > 32) {
                /* 显示名优先用配置的(腾讯返回的是"纳指100ETF-Invesco"这种全称,
                 * 卡上要的是 QQQ / 159501), 对不上才退回腾讯的名字 */
                const char * disp = dash_stock_display_name(code);
                if(disp != NULL && disp[0] != 0) {
                    snprintf(out[count].name, sizeof(out[count].name), "%s", disp);
                } else {
                    dash_gbk_to_utf8((const uint8_t *)field[1], out[count].name, sizeof(out[count].name));
                }
                snprintf(out[count].price, sizeof(out[count].price), "%s", field[3]);
                snprintf(out[count].pct, sizeof(out[count].pct), "%s%%", field[32]);
                float change = (float)atof(field[31]);
                out[count].direction = (change > 0.001f) ? 1 : ((change < -0.001f) ? -1 : 0);
                ESP_LOGI(TAG, "股票: %s %s %s", out[count].name, out[count].price, out[count].pct);
                count++;
            }
            p = end + 1;
        }
    }

    free(buf);
    return count > 0 ? count : -1;
}