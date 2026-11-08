# 看板服务器接口契约

看板在运行时向服务器要一份完整配置。
**固件里只有占位符**——HA 长令牌、天气 Key、股票代码、服务器地址全部由这个接口下发。

服务器地址本身是唯一需要"在设备上先填"的东西(见 `FLASHING.md`)，
拿到地址之后，剩下所有东西都走这个接口。

---

## 1. 配置下发

```
GET {server_base}/device/config?device_id=<设备身份>
```

- `server_base`：形如 `http://192.168.x.x:8766`，用户在 Vibe Mode 共享信息里填 `dash_server`
- `device_id`：设备身份，默认取 STA MAC 后 3 字节(小写 hex)，
  也可以在共享信息里用 `dash_device` 覆盖

### 响应

```json
{
  "title": "客厅",
  "ha_url": "http://homeassistant.local:8123",
  "ha_token": "<HA 长令牌>",
  "qweather_key": "<和风天气 API Key>",
  "qweather_host": "devapi.qweather.com",
  "qweather_location": "101010100",
  "stock_codes": "sh600519,sz000001",
  "stock_names": "贵州茅台,平安银行",
  "devices": [
    { "name": "客厅灯带", "entity": "light.study_light", "domain": "light" },
    { "name": "书房空调", "entity": "fan.study_ac",      "domain": "fan"   }
  ]
}
```

| 字段 | 必填 | 说明 |
|---|---|---|
| `title` | 否 | 顶栏标题，缺省用「家庭看板」 |
| `ha_url` | 是 | Home Assistant 基地址 |
| `ha_token` | 是 | HA 长令牌(`Authorization: Bearer <token>`) |
| `qweather_key` | 否 | 和风天气 Key，缺省天气页显示 `--` |
| `qweather_host` | 否 | 和风域名，缺省 `devapi.qweather.com` |
| `qweather_location` | 否 | 城市 ID，如 `101010100` |
| `stock_codes` | 否 | 股票代码，逗号分隔，腾讯行情格式(`sh`/`sz` 前缀) |
| `stock_names` | 否 | 显示名，与代码一一对应；不对上就退回腾讯返回的名字 |
| `devices` | 否 | 设备清单，最多 **5** 条；多了只取前 5 条 |

`domain` 取 `light` 或 `fan`；认不出来的按 `light` 处理。
`light` → 亮度百分比，`fan` → 风速百分比。

### 落盘

拿到后写进 NVS(`dashcfg` 命名空间)，**一个字段一个键**：

| 键 | 内容 |
|---|---|
| `base` | 服务器地址 |
| `device` | 设备身份 |
| `title` / `haurl` / `hatoken` / `qwkey` / `qwhost` / `qwloc` / `codes` / `names` | 对应上面各字段 |
| `devs` | 设备清单，定长 blob(每条 89 字节 = 显示名 24 + 实体 ID 64 + 域 1) |

> 为什么不拼成一个大串存？一是串太长容易溢出，二是字段里只要带了 `|` 就会串台
> (HA 令牌、URL 都可能带)。一个字段一个键，长度天然受字段数组大小约束。

### 失败怎么办

- 服务器拉不到 → 用 NVS 里上一次的结果顶着，界面顶栏显示「缓存」
- NVS 也是空的 → 天气/股票/服务器状态显示 `--`，设备清单为空(设备页五行隐藏)
- 每次 WiFi 重连后都会重新拉一次配置

---

## 1.5 服务端怎么决定下发什么

同一个接口服务两类请求，**靠 `device_id` 参数区分**：

| 请求 | 返回 |
|---|---|
| `GET /device/config?device_id=xxx` | 上面那份运行时配置（新固件用） |
| `GET /device/config`（不带参数） | 网页看板自己用的布局配置（老接口，不受影响） |

服务器侧（`dashd.py`）的下发规则：

| 内容 | 来源 |
|---|---|
| `ha_url` / `ha_token` | 服务器的 `secrets/ha.json` |
| `qweather_key` | 服务器的 `secrets/qweather.json` |
| `qweather_host` | 缺省 `api.qweather.com`（该凭据绑定的 Host） |
| `qweather_location` | 服务器 `config.json` 的 `device.location_id` |
| `stock_codes` / `stock_names` | 服务器 `config.json` 的 `quotes.symbols` |
| `devices` | 服务器 `config.json` 的 `touch.items`（最多取前 5 条） |

`domain` 由服务器判断：实体 ID 以 `fan.` 开头或控件类型是 `fan` → `fan`，其余 → `light`。

### 多块板子：按 device_id 分发

服务器 `config.json` 里可以加 `device_profiles`，键就是板子上的 `device_id`：

```json
{
  "device_profiles": {
    "livingroom": {
      "title": "客厅",
      "devices": [
        { "name": "客厅灯带", "entity_id": "light.unknown" },
        { "name": "卧室风扇", "entity_id": "fan.dmaker_cn_657655659_p5c_s_2_fan" }
      ]
    },
    "bedroom": {
      "title": "卧室"
    }
  }
}
```

- `devices` 一旦给了就整个替换默认清单；不给就用服务器默认那组
- `title` 不给就显示「家庭看板」
- 没匹配到 profile 的板子走默认值，不影响别的板子

---

## 2. 服务器系统状态

看板服务器自己也要暴露一个状态接口(从 7C 那边沿用)：

```
GET {server_base}/device/sysstat
```

```json
{
  "hosts": [
    { "n": "客厅 NAS", "ip": "192.168.x.x", "cpu": 12.5, "mem": 46.0, "steal": 0.0, "ok": true }
  ],
  "steal": 0.0,
  "load": 1.42,
  "mem": 58,
  "disk": 63,
  "disk_free": 118,
  "down": 348
}
```

| 字段 | 说明 |
|---|---|
| `hosts[].n` | 主机名 |
| `hosts[].ip` | 内网地址 |
| `hosts[].cpu` / `mem` / `steal` | CPU / 内存 / steal 占用百分比 |
| `hosts[].ok` | 这台探到了没有 |
| `load` | 1 分钟负载 |
| `mem` / `disk` / `disk_free` | 服务器整体内存占用 % / 磁盘占用 % / 磁盘剩余 GB |
| `down` | 下载速率 KB/s |

最多显示 3 台主机(`DASH_HOST_MAX`)。
看板会顺便测一次往返延迟，填到「延迟 xx ms」。

---

## 3. 看板自己直接调的外部接口

这些不走看板服务器，板子直连：

| 接口 | 用途 |
|---|---|
| `{qweather_host}/v7/weather/now` | 实况天气 |
| `{qweather_host}/v7/weather/3d` | 三日预报 |
| `{qweather_host}/v7/air/now` | 空气质量 |
| `qt.gtimg.cn/q=sh600519,sz000001` | 腾讯股票行情(GBK 编码，板子侧转 UTF-8) |
| `{ha_url}/api/states/{entity_id}` | HA 状态查询 |
| `{ha_url}/api/services/{domain}/{service}` | HA 控制(切灯/调风速) |

天气接口返回带 `gzip`，板子侧自己解(miniz)。

---

## 4. 安全提醒

- **不要把 `ha_token` / `qweather_key` 写进固件仓库**，它们只经过这个接口
- 看板服务器要能分辨 `device_id` 是哪块板，不同的板下发不同的配置
- 板子只把配置写进 NVS，不上云、不外发
- 日志里只打 SSID，**不打 WiFi 密码、不打 HA Token**
