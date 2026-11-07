# main: 设备组装根

按顺序初始化平台服务、启动 Recovery、加载 UI bundle、准备显示、启动 GSP、
最后挂上应用控制器。它不处理输入、动画和绘制规则，也不导出头文件。

## 启动顺序

顺序有讲究：

1. `esp_iris_boot_probe()` —— Iris 启动前先探崩溃，
   免得应用初始化崩了进不去 Recovery
2. `nvs_flash_init()`
3. `app_net_start()` —— 网络栈先建起来（配没配网都要建），
   否则 ESP-Iris 的 TCP 通道没有落脚点。没配网不算致命错误，
   界面上会显示「未配网」
4. `iris_ota_support_start()` —— Recovery 必须一直可达，哪怕后面的 UI 起不来
5. `ui_bundle_open()` → `app_display_init()` → `esp_gsp_esp_lcd_start()`
   → `iris_screen_mirror_attach()`
6. `app_dash_create()` —— 最后把数据层挂上

## 依赖

私有依赖：`app_display`、`app_dash`、`app_net`、`dash_data`、Iris、Recovery、
bundle 加载、屏幕镜像、NVS 和日志。共享的产品/平台组件保持原名。
中央依赖 pin / override 写在 `idf_component.yml`。

## 所有权

ESP-IDF 调用一次 `app_main()`，无参数，启动完就返回。
组装根只持有自己拥有的不透明句柄（`ui`、`controller`），
UI/控制器的生命周期一直到重启。bundle 或显示初始化失败时，
**Recovery 保持可达**；其它不可恢复的初始化错误走 ESP-IDF 的错误检查策略。

用工作区的启动器构建/运行，见根目录 README。
