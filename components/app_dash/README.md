# app_dash

家庭看板的 UI 控制器。设备上和 PC 模拟器上是**同一份实现**。

## 职责

三条线：

1. **定时器(1 秒)** —— 刷顶栏时钟、跟页码指示器
2. **数据回调** —— 数据层每拿到一份新快照就调这里，把值翻译成 GSP 的 setter 调用
3. **事件回调** —— 刷新按钮、五个设备滑块

所有 setter 都是往 GSP 渲染任务队列里投命令，不阻塞，
所以在数据层任务上下文里调也安全。但回调本身不能做耗时的事：
网络请求都在数据层里，这里只做「排队写界面」。

## 依赖

只依赖**公开的 ESP-GSP API** 和从 `ui/main.json` 生成的绑定，
不 import display / BSP / `main`。CMake 负责生成 bundle，
生成的绑定不外泄。`ui/` 下是作者维护的场景和字体。

`host/` 是可选的 PC 适配层，**不进设备固件**：

- `platform_pc.c` —— 模拟器后端入口
- `dash_data_stub.c` —— 模拟器用的假数据层（真机上换成 `components/dash_data`）

两边都用 `include/` 和同一份控制器实现，所以界面行为只有一个来源。

## 用法

```c
app_dash_handle_t controller = NULL;
esp_gsp_err_t err = app_dash_create(ui, &controller);
/* ESP_GSP_OK 时, ui 和 controller 要活到 GSP 不再派发事件为止 */
```

控制器借用 `ui`，占用它唯一的应用事件回调，拥有自己的状态和定时器。
`ui` 不能再有别的应用事件所有者。在启动任务里创建一次（Host 侧在 poll 之前），
不要并发调用。

## 生命周期与所有权

- `app_dash_create()` 成功后，控制器拥有：`ui` 的借用引用、事件回调、定时器
- 拆除顺序：**先 `app_dash_destroy()`（会先停数据层、再摘回调），最后关 UI**
- `app_dash_destroy()` 之后 `*handle` 被置空

## 字体

`CMakeLists.txt` 里挂了 `dash_fonts` 自定义目标：
场景 JSON 的中文变了，编译时会自动重跑 `tools/fonts/gen_dash_fonts.py`
重新裁字体子集，再交给 gspc 烘焙。改中文不用手动生成字体。
