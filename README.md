# 家庭看板 · ESP-Mosaico 正式版

把 7C 板(Waveshare ESP32-S3-Touch-LCD-7C, 800×480)上的家庭看板，
搬到乐鑫 **ESP-Mosaico** 套件(2.16" 480×480 AMOLED)上。

本工程是**正式版**：界面是最终版面，数据层全部接通，
HA Token / 天气 Key 等敏感信息**运行时由服务器下发，不进固件**。

---

## 一、硬件与平台

| 项目 | 参数 |
|---|---|
| SoC | **ESP32-S31NRV16**(RISC-V 双核 @320MHz, 512KB SRAM + 外接 PSRAM) |
| 屏幕 | 2.16" **480×480** LTPS-AMOLED，CO5300 驱动，QSPI |
| 触摸 | CST9220(电容) |
| Flash | **16MB** |
| 工具链 | ESP-IDF **v6.2.0** @ `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`，target `esp32s31` |

分区表沿用 Mosaico 平台布局（**不要改**）：
`otadata / phy_init / sysmeta / vibe_mode / coredump / nvs / main_app(13.5MB) / ui_apps(1MB)`。

---

## 二、界面：三页

480×480 方形屏，顶部一条 84px 顶栏，下面 PageFlow 三页（左右滑动切换）。

| 页 | 内容 |
|---|---|
| **天气页** | 温度环(arc −20~50℃)、大号温度、天气状况、最高最低温、湿度/空气/日出三卡、空气质量卡、未来三天卡 |
| **设备页** | 5 行设备：名称 + 当前值 + 亮度/风速滑块。滑块松手即下发到 Home Assistant |
| **数据页** | 股票卡(4 行：名称/价格/涨跌幅)、服务器卡(3 台主机 + 负载/磁盘/延迟/下载) |

两个细节：

- **陈旧态用遮罩层实现**：GSP 的 label 不支持动态主题色，`shape` 也不支持 bind，
  所以"数据是缓存/离线"用一个半透明 `container` + `visible` bind 盖住整页，
  而不是把文字变淡。
- **顶栏在 PageFlow 之外**，所以切页时顶栏不动，只有内容区翻页。

---

## 三、架构

```
main/main.c
   │
   ├── app_net      WiFi 接入(凭据来自 Vibe Mode 共享信息, 固件里没有密码)
   ├── dash_data    数据层: 天气 / 股票 / 服务器状态 / Home Assistant
   │     └── dash_cfg  运行时配置: 向服务器要一份完整配置, 落 NVS
   └── app_dash     UI 控制器: 快照 → GSP setter, 滑块 → 数据层
```

### 敏感信息为什么不进固件

HA 长令牌、和风天气 Key、股票代码、服务器地址——这些全部由看板服务器在运行时下发：

```
GET {server_base}/device/config?device_id=<设备身份>
```

拿到后写进 NVS(`dashcfg` 命名空间，**每个字段一个键**)，
下次开机先用 NVS 顶着，联网后再刷新一份。
固件二进制里只有占位符和字段名。

详见 [`docs/SERVER_CONFIG.md`](docs/SERVER_CONFIG.md)。

### 设备身份 = 多环境配置切换

固件只有一份。装在客厅那块板显示客厅的，装在卧室那块显示卧室的，
区别只在于**设备身份**（默认取 STA MAC 后 3 字节，也可以用共享信息 `dash_device` 覆盖）
对应服务器上哪一套配置。**不需要为第二个屏幕重新编译。**

### 配网方式(Mosaico 专用，不用 SoftAP)

| 配什么 | 在哪配 | 固件怎么读到 |
|---|---|---|
| WiFi 账号密码 | Vibe Mode 的 WiFi 设置 | `mosaico_shared_get("wifi_ssid"/"wifi_password")` |
| 看板服务器地址 | Vibe Mode → 设置 → 共享信息，键 `dash_server` | `mosaico_shared_get("dash_server")` |
| 设备身份(可选) | 同上，键 `dash_device` | `mosaico_shared_get("dash_device")` |

固件里**不存密码**，只读 Vibe Mode 写进 `sysmeta/shared` 的保留字段。
详见 [`docs/FLASHING.md`](docs/FLASHING.md) 的"首次配置"一节。

---

## 四、中文字体

GSP 把文字在**编译期**烘焙成点阵(GFB)，所以必须自带中文字体。

```
场景 JSON 的 text / font_charset ─┐
                                  ├─► tools/fonts/gen_dash_fonts.py ─► NotoSansSC-DashSubset.ttf
NotoSansSC-Regular.otf ───────────┘                                          │
                                                                             ▼
                                                            gspc 烘焙成 dash_font0..11.gfb
```

已挂进 CMake(`components/app_dash/CMakeLists.txt`)，**改了中文不用手动生成字体**：

```cmake
add_custom_command(OUTPUT "${DASH_FONT_SUBSET}" ...)
add_custom_target(dash_fonts DEPENDS "${DASH_FONT_SUBSET}")
add_dependencies(${COMPONENT_LIB}_gsp_bundle dash_fonts)   # 保证 gspc 在字体之后跑
```

### 运行时中文怎么保证不空白

场景里的 `font_charset` 除了静态文字，还**按域声明了运行时可能出现的字**
（房间名/设备类型/天气状况/空气质量等级/预报星期/股票名/主机名/指标词）。
gspc 会按字号分别栅格化，所以这些字只进用得上的那几个字号。

当前规模：**484 字 / 160KB TTF / 306KB 烘焙进 bundle**，
`ui_apps` 分区用了 370KB / 1MB，还有余量。

> 如果服务器下发的设备名包含字集外的字，屏幕上会是空白。
> 两种办法：① 在场景 JSON 对应标签的 `font_charset` 里补字（改完重编即可，字体自动重生成）；
> ② 改用纯 ASCII 的设备名。

依赖：IDF 的 python venv 里要有 fonttools

```sh
~/.espressif/python_env/idf6.2_py3.12_env/bin/python -m pip install fonttools==4.68.0
```

---

## 五、目录结构

```
projects/home_dashboard/
├── main/
│   ├── main.c                       启动组装: iris_probe → nvs → app_net → iris_ota → GSP → app_dash
│   ├── CMakeLists.txt
│   └── idf_component.yml
├── components/
│   ├── app_dash/                    ★ UI 控制器
│   │   ├── app_dash.c               定时器(时钟/页码) + 数据回调 + 事件回调(刷新/滑块)
│   │   ├── include/app_dash.h
│   │   ├── ui/main.json             ★ 场景: 版面 / 配色 / 静态文字 / 控件
│   │   ├── ui/fonts/                (生成的子集字体, 已 gitignore)
│   │   ├── host/                    PC 模拟器后端(不是设备代码)
│   │   │   ├── platform_pc.c
│   │   │   └── dash_data_stub.c     模拟器用假数据(真机上是 dash_data)
│   │   └── CMakeLists.txt           ★ 字体自动生成接在这里
│   ├── dash_data/                   数据层(从 7C 搬过来 + 重构)
│   │   ├── include/dash_types.h     共享数据结构(不依赖 ESP-IDF, 模拟器也能用)
│   │   ├── include/dash_cfg.h       运行时配置(服务器下发)
│   │   ├── include/dash_data.h      统一入口: 快照 + 通知
│   │   ├── src/dash_*.c             http / weather / stock / sysstat / ha / cache / gbk
│   │   └── CMakeLists.txt
│   ├── app_net/                     WiFi 接入
│   └── app_display/                 BSP 显示适配
├── tools/fonts/gen_dash_fonts.py    ★ 中文字体子集生成器
├── docs/
│   ├── SERVER_CONFIG.md             看板服务器要实现的接口契约
│   └── FLASHING.md                  ★ 烧录 + 首次配置步骤
└── build/                           编译产物
```

`dash_data` 的头文件刻意**不依赖 ESP-IDF**，所以界面层能原样编到 PC 上跑模拟器。

---

## 六、常用命令

```sh
cd /home/openclaw/esp/esp-mosaico-vibe
export IDF_PATH=/home/openclaw/esp/idf-mosaico
. /home/openclaw/esp/idf-mosaico/export.sh
export PATH="$HOME/.espressif/tools/cmake/4.0.3/bin:$HOME/.espressif/tools/ninja/1.12.1:$PATH"

# 场景静态检查(0 error / 0 warning 才往下走)
~/.cache/esp-mosaico/gspc/0.6.1/gspc diagnose --platform-soc esp32s31 --psram \
    projects/home_dashboard/components/app_dash/ui/main.json

# 编译固件
python3 submodule/esp-mosaico-utils/mosaico-tools/skills/idf-low-noise-build/scripts/idf_low_noise_build.py \
    --project projects/home_dashboard build

# 模拟器: 跑 C 后端 4 秒, 看定时器/回调/setter 是否正常
python3 mosaico.py project sim --project projects/home_dashboard --headless --duration 4

# 模拟器: 出 480×480 图
python3 mosaico.py project sim --project projects/home_dashboard --headless \
    --dump-ppm /tmp/out.ppm
```

真机安装(设备插上 USB 之后，见 `docs/FLASHING.md`)：

```sh
python3 mosaico.py recover                                  # 空白/未验证设备首次
python3 mosaico.py iris system-update --project projects/home_dashboard
python3 mosaico.py iris screenshot out.png --json           # 真机截图验收
```

---

## 七、当前状态(2026-11-07)

| 环节 | 状态 | 证据 |
|---|---|---|
| 场景编译 | ✅ | `gspc diagnose` → `[]`（0 error / 0 warning） |
| 中文渲染 | ✅ | 模拟器 480×480 出图，三页中文全部正常 |
| 固件编译 | ✅ | `build/home_dashboard.bin` = 2,003,552 B，`-Wall -Werror` 零警告 |
| GSP bundle | ✅ | `build/ui_apps.bin` = 370,064 B（分区上限 1,048,576 B） |
| 运行期逻辑 | ✅ | 模拟器：`tick=5 snapshot=1 last_error=0`（setter 全部成功） |
| 烧录包 | ✅ | `build/home_dashboard-system-update.irisfw` = 1,419,420 B |
| 真机烧录 | ⏳ | **设备不在这台机器上**（无 `/dev/ttyACM*`），由用户自己烧 |

### 还没验证的(需要真机)

1. 真机上的实际渲染效果(模拟器只验了场景和 setter，没验 AMOLED 色彩/刷新率)
2. Vibe Mode 配网后能否稳定拿到 `dash_server`
3. 服务器 `/device/config` 接口的联调(接口契约已写好，见 `docs/SERVER_CONFIG.md`)
4. HA 长令牌的连通性与滑块回控
