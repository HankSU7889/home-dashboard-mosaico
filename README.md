# 家庭看板 · ESP-Mosaico 版

把 7C 板(Waveshare ESP32-S3-Touch-LCD-7C, 800×480)上的家庭看板,
搬到乐鑫 **ESP-Mosaico** 套件(2.16" 480×480 AMOLED)上。

---

## 一、这块板子是什么(从 PDF 规格书 + 原理图读出来的)

| 项目 | 参数 | 来源 |
|---|---|---|
| SoC | **ESP32-S31NRV16**(RISC-V, 不是 S3) | 原理图 |
| 屏幕 | 2.16" **480×480** LTPS-AMOLED, CO5300 驱动, QSPI | 面板规格书 H0216F002AMT004-1 V1 |
| 触摸 | CST9220(电容) | 面板规格书 + BSP 依赖 |
| 显示区 | 38.39 × 38.39 mm(**方形**, 不是圆形) | 面板规格书 |
| 音频 | ES8311 编解码 + NS4150B 功放 + 麦克风 | 原理图 |
| 传感器 | BMI270 六轴 + BMM150 ×2 磁力计 | 原理图 |
| 其它 | LMA2718B331 震动马达 / 4.2V 65mAh 电池 / HUSB320 USB Hub | 原理图 |
| Flash | **16MB**(7C 那块是 32MB) | sdkconfig.defaults |

> 注意: Mosaico 的屏是**方形 480×480**, 布局可以照搬 7C 的卡片式,
> 不需要为了圆形留黑边。

---

## 二、和 7C 固件的差异(移植时必须知道)

| 维度 | 7C(现有) | Mosaico(本工程) |
|---|---|---|
| SoC | ESP32-S3 | **ESP32-S31** |
| 屏幕 | 800×480 LCD | **480×480 AMOLED** |
| UI 写法 | 手写 LVGL v9 `lv_obj` / tileview | **GSP 场景 JSON + 生成式 C API** |
| 编译 | IDF v6.1 | **IDF v6.2 @ `7b9cc1ac`**(工作区固定) |
| Flash | 32MB | 16MB |
| 安装 | 自己写的 merged bin + OTA | `.irisfw` / Ideas ZIP, 走 `mosaico.py` |

**结论: UI 层要重写, 数据层可以搬。**

- `dash_ha.c` / `dash_net.c`(HTTP 客户端)是可移植的纯 C, 搬过来即可;
- `dash_ui.c`(LVGL v9 手写界面)整个作废, 改成 `ui/main.json` 场景 + `app_hello.c` 控制器;
- `dash_config*.h` 的"配置即一个头文件"思路保留, 换成 GSP 工程里的 `ui/main.json` + 一个 `dash_config.h`。

---

## 三、已经跑通的部分(2026-11-07)

| 环节 | 状态 | 证据 |
|---|---|---|
| Mosaico 工作区 | ✅ | `/home/openclaw/esp/esp-mosaico-vibe`(官方 `esp-mosaico-vibe`) |
| ESP-IDF | ✅ | `/home/openclaw/esp/idf-mosaico`, v6.2.0, SHA `7b9cc1ac79f8…` |
| GSP 编译器 | ✅ | gspc 0.6.1 (`~/.cache/esp-mosaico/gspc/0.6.1/gspc`) |
| 场景编译 | ✅ | `gspc diagnose` 0 error / 0 warning |
| 中文渲染 | ✅ | 模拟器 480×480 出图, 中文全部正常 |
| 固件编译 | ✅ | `build/home_dashboard.bin` ≈ 1.3MB |
| 运行期逻辑 | ✅ | 模拟器跑 4 秒: `tick=4, last_error=0`(定时刷新 + setter 全部成功) |
| 真机烧录 | ❌ | **设备没接在这台机器上**(只有 QEMU 平板, 无 `/dev/ttyACM*`) |

### 中文是怎么解决的(重点)

GSP 场景里的文字是**编译期烘焙**成点阵(GFB)的, 所以必须自带中文字体。

方案: 用 fontTools 把 Noto Sans SC 按场景里实际用到的字裁成子集。

```
场景 JSON 里的 text ──┐
                      ├─► tools/fonts/gen_dash_fonts.py ─► NotoSansSC-DashSubset.ttf (≈90KB)
NotoSansSC-Regular.otf ┘                                          │
                                                                     ▼
                                                        gspc 烘焙成 hello_font0..7.gfb
```

而且已经接进 CMake(`components/app_hello/CMakeLists.txt`), **改中文不用手动生成字体**:

```cmake
add_custom_command(OUTPUT "${DASH_FONT_SUBSET}" ...)
add_custom_target(dash_fonts DEPENDS "${DASH_FONT_SUBSET}")
add_dependencies(${COMPONENT_LIB}_gsp_bundle dash_fonts)   # 保证 gspc 在字体之后跑
```

依赖: IDF 的 python venv 里要有 fonttools

```sh
~/.espressif/python_env/idf6.2_py3.12_env/bin/python -m pip install fonttools==4.68.0
```

> 运行期动态文字(比如 HA 设备名)如果字表里有, 用 `font_charset` 声明;
> 如果要**完全任意**的中文, 走 `DYNAMIC_FONT`(FreeType 运行时加载 TTF),
> 但那要额外分区存一份完整字体。

---

## 四、目录结构

```
projects/home_dashboard/
├── main/
│   ├── main.c                     启动 / 依赖组装(模板给的)
│   ├── idf_component.yml          依赖: bsp / esp_iris / esp-gsp==1.5.1 / lvgl==9.5.0
│   └── CMakeLists.txt
├── components/
│   ├── app_hello/
│   │   ├── app_hello.c            ★ UI 控制器: 定时刷新 + 点击回调 + setter 写回
│   │   ├── include/app_hello.h
│   │   ├── ui/
│   │   │   ├── main.json          ★ 场景: 版面 / 配色 / 静态文字 / 控件
│   │   │   └── fonts/             (生成的子集字体, 已 gitignore)
│   │   ├── host/                  PC 模拟器后端(不是设备代码)
│   │   └── CMakeLists.txt         ★ 字体自动生成接在这里
│   └── app_display/               BSP 显示适配(模板给的)
├── tools/fonts/gen_dash_fonts.py  ★ 中文字体子集生成器
├── sdkconfig.defaults             16MB flash / CO5300 / PSRAM 等
└── build/                         编译产物
```

---

## 五、常用命令

```sh
cd /home/openclaw/esp/esp-mosaico-vibe
export IDF_PATH=/home/openclaw/esp/idf-mosaico
. /home/openclaw/esp/idf-mosaico/export.sh

# 场景静态检查(0 error 才往下走)
~/.cache/esp-mosaico/gspc/0.6.1/gspc diagnose --platform-soc esp32s31 --psram \
    projects/home_dashboard/components/app_hello/ui/main.json

# 编译固件
python3 submodule/esp-mosaico-utils/mosaico-tools/skills/idf-low-noise-build/scripts/idf_low_noise_build.py \
    --project projects/home_dashboard build

# 模拟器: 跑 C 后端 4 秒, 看定时器/回调是否正常
python3 mosaico.py project sim --project projects/home_dashboard --headless --duration 4

# 模拟器: 只渲染场景, 出 480×480 图
python3 mosaico.py project sim --project projects/home_dashboard --headless \
    --frames 30 --dump-ppm /tmp/out.ppm
```

真机安装(设备插上 USB 之后):

```sh
python3 mosaico.py recover                                  # 空白/未验证设备首次
python3 mosaico.py iris system-update --project projects/home_dashboard
python3 mosaico.py iris screenshot out.png --json           # 真机截图验收
```

---

## 六、还没做的(需要拍板)

1. **界面设计**: 现在这版只是"技术验证场景"(把 7C 的四张卡片压到一块 480×480 上),
   不是最终设计。要正式做一版卡片布局吗?
2. **数据接入**: HA / 天气 / 股票 现在是写死的演示值。
   真接的话, 要不要把 7C 的 `dash_ha.c` / `dash_net.c` 直接搬过来?
3. **WiFi 配网**: Mosaico 走 ESP-Iris 配网(`iris test recovery-wifi`),
   WiFi 账号密码不放在固件里, 和 7C 的做法不一样, 配置流程要重新设计。
4. **HA Token 等敏感信息**: 7C 那版是编译期宏 + `dash_config_secrets.h`;
   Mosaico 这边建议改成 NVS / Kconfig 运行时下发, 别进固件二进制。
5. **设备不在本机**: 烧录/真机验收需要把 Mosaico 插到这台机器(或你自己的电脑)。
