# 烧录与首次配置

> ⚠️ 板子不在开发机上，**烧录由用户自己执行**。本文只给产物和步骤。

---

## 一、烧录产物

已构建好的包(在 `projects/home_dashboard/build/` 下)：

| 文件 | 大小 | 说明 |
|---|---|---|
| `home_dashboard-system-update.irisfw` | 1,419,420 B | **烧录用这个**，ZIP 格式的 system-update 包 |
| `home_dashboard.bin` | 2,003,552 B | 应用固件(包里的 `ota_0.bin`) |
| `ui_apps.bin` | 370,064 B | GSP 界面资源包(包里的 `ui_apps.bin`) |
| `partition-table.bin` | 4,096 B | 分区表(包里的 `partition-table.bin`) |

`.irisfw` 里的 `manifest.json` 已经写好了每个分区的落点：

| 组件 | 目标偏移 | 大小 | SHA-256 |
|---|---|---|---|
| `partition-table.bin` | `0x8000` | 4,096 | `3c26b245870110b9…` |
| `ota_0.bin` | `0x210000` | 2,003,552 | `1f5ccbdbbdfd9e87…` |
| `ui_apps.bin` | `0xF00000` | 370,064 | `b80cc9ef0629a04b…` |

目标：`chip_id=32`(ESP32-S31)，`flash_size=16777216`(16MB)，`minimum_recovery_version=0.2.0`。

---

## 二、烧录步骤

### 前提

- 板子用 USB 连上电脑
- ESP-Mosaico 工作区齐全(`/home/openclaw/esp/esp-mosaico-vibe`)
- IDF 环境就位：

```sh
export IDF_PATH=/home/openclaw/esp/idf-mosaico
. /home/openclaw/esp/idf-mosaico/export.sh
export PATH="$HOME/.espressif/tools/cmake/4.0.3/bin:$HOME/.espressif/tools/ninja/1.12.1:$PATH"
cd /home/openclaw/esp/esp-mosaico-vibe
```

### 情况 A：板子是空白的 / 从没验证过

```sh
python3 mosaico.py recover
```

这一步写入 Vibe Mode 基础固件并把设备恢复到可安装应用的状态。
**必须先做这步**，否则 `system-update` 会因为设备不在 Vibe Mode 而失败。

### 情况 B：板子里已有 Vibe Mode(正常情况)

直接装：

```sh
python3 mosaico.py iris system-update \
    --project projects/home_dashboard
```

### 情况 C：用已经构建好的包(不重新编译)

```sh
python3 mosaico.py iris system-update \
    --bundle projects/home_dashboard/build/home_dashboard-system-update.irisfw
```

> **不要用 `idf.py flash` / `esptool` 自己去写偏移。**
> 这个工作区规定设备操作一律走 `python mosaico.py`：
> 它会自己处理 Vibe Mode 切换、兼容性校验、分区前缀保护。
> 手写偏移会绕过这些检查。

---

## 三、首次配置(装完之后)

板子启动后需要两样东西，都**不在固件里**。

### 1. WiFi

在 Vibe Mode 里配置 WiFi(Vibe Mode → WiFi 设置)。
板子会从共享信息里读 `wifi_ssid` / `wifi_password` 自动连接。
**固件里不存密码。**

### 2. 看板服务器地址

Vibe Mode → **设置 → 共享信息**，添加一条：

| 键 | 值 | 必填 |
|---|---|---|
| `dash_server` | `http://<你的服务器IP>:8766` | **是** |
| `dash_device` | 设备身份，如 `livingroom` | 否 |

- 键名规则：1~15 个 ASCII 小写字母/数字/下划线
- 值最长 256 字节
- `dash_server` 末尾不要带 `/`（带了也会被自动去掉，但别写错）
- `dash_device` 不填的话，默认取 WiFi STA MAC 的后 3 字节（小写 hex），
  服务器据此决定下发哪一套配置

> 服务器要实现哪个接口，见 [`SERVER_CONFIG.md`](SERVER_CONFIG.md)。

---

## 四、验收

```sh
# 实时身份 / Boot ID / 固件模式
python3 mosaico.py iris device-status --json

# 截图(直接拿设备上的画面)
python3 mosaico.py iris screenshot /tmp/dash.png --json

# 看日志
python3 mosaico.py iris logs
```

验收标准(全部满足才算装好)：

1. 同一个 Device ID 走完 正常 → Vibe Mode → 正常，两次重启的 Boot ID 都是新的
2. Vibe Mode 就绪、目标固件健康
3. 设备屏幕上顶栏显示「在线」而不是「离线」/「缓存」
4. 天气页温度环有数值，设备页五行都有名字，滑块能拖动
5. 拖动滑块后 Home Assistant 里的设备状态跟着变

---

## 五、出问题怎么办

| 现象 | 处理 |
|---|---|
| `No available ESP-Iris USB device was found` | 板子没连上 / 不在 Vibe Mode。先 `iris list` 看一眼；空白设备走 `recover` |
| `partition_layout_mismatch` | 分区表和设备不一致。**不要改分区表绕过去**，改用 `system-update` |
| 顶栏一直「离线」 | 板子还没连上 WiFi，或者 `dash_server` 没填/填错 |
| 天气页全是 `--` | 服务器没配 `qweather_key`，或者板子没拿到配置(看 `iris logs` 里 `dash_cfg` 的日志) |
| 设备页五行空着 | 服务器没下发 `devices`，或者实体 ID 不对 |
| 中文显示空白 | 设备名里有字集外的字，见 README「运行时中文怎么保证不空白」 |

---

## 六、重新构建

改了代码或场景之后：

```sh
cd /home/openclaw/esp/esp-mosaico-vibe

# 编译
python3 submodule/esp-mosaico-utils/mosaico-tools/skills/idf-low-noise-build/scripts/idf_low_noise_build.py \
    --project projects/home_dashboard build

# 重新打包 system-update
python3 mosaico.py iris system-update --project projects/home_dashboard
```

场景 JSON 改了中文不用手动生成字体——CMake 的 custom command 会自动跑
`tools/fonts/gen_dash_fonts.py` 重新裁子集。
