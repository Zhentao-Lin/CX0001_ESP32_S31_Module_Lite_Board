# 01 · GPIO 输出 - 板载 LED 闪烁

**示例工程**：[`03.Sketch/01_GPIO_LED`](../03.Sketch/01_GPIO_LED)

用 ESP-IDF GPIO 驱动板载 LED（GPIO38）闪烁。

> 先读 [00_开发环境.md](00_开发环境.md)。01～03 **无串口输出**，只看 LED。

## 本节目标

- 板载 LED 引脚；GPIO 输出：复位 → 方向 → 电平；
- 用 Kconfig 改引脚，便于换板。

## 用到的硬件

- 板载 LED（GPIO38，高电平亮）。

## 工程配置

1. VS Code 打开 `03.Sketch/01_GPIO_LED`。
2. 首次：`idf.py --preview set-target esp32s31`。
3. `sdkconfig.defaults`：esp32s31、16MB Flash/PSRAM、自定义分区表、`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`。
4. 可选 menuconfig → **Example Configuration**：`Blink GPIO`（38）、`Blink interval`（300 ms）。

源码：`main/gpio_led_main.c`、`main/Kconfig.projbuild`。

## 编译与下载

**VS Code**：选 COM 口 → Build → Flash（不必 Monitor）。

**命令行**：

```powershell
idf.py --preview build
idf.py --preview -p <COM口> flash
```

## 编程效果

LED 每 **0.3 s** 亮灭一次；无串口日志。

## 代码逐段讲解

源码主文件：[`main/gpio_led_main.c`](../03.Sketch/01_GPIO_LED/main/gpio_led_main.c)。工程里还有 `main/Kconfig.projbuild`（生成 `CONFIG_BLINK_GPIO` / `CONFIG_BLINK_DELAY_MS`）、`main/CMakeLists.txt`（把 `gpio_led_main.c` 编进 `main` 组件）。

### 1. 文件头与包含关系

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "driver/gpio.h"
```

- **`freertos/FreeRTOS.h`**：FreeRTOS 内核类型与 `configTICK_RATE_HZ` 等配置。
- **`freertos/task.h`**：任务 API；本节只用 `vTaskDelay()`，代码跑在默认的 **`app_main` 任务**里（IDF 启动后自动创建）。
- **`esp_err.h`**：`esp_err_t` 与 **`ESP_ERROR_CHECK`** 宏——若表达式非 `ESP_OK`，打印错误并 **`abort()`**，便于第一次在开发板上定位配置问题。
- **`driver/gpio.h`**：通用 GPIO 驱动，与具体芯片引脚号无关。

### 2. Kconfig 宏（换板只改 menuconfig）

```c
#define BLINK_GPIO  CONFIG_BLINK_GPIO
#define BLINK_DELAY_MS  CONFIG_BLINK_DELAY_MS
```

`CONFIG_BLINK_GPIO` 在 `Kconfig.projbuild` 里默认 **38**；编译时由 Kconfig 系统写入 `sdkconfig`，再变成 C 宏。改引脚不必动 `.c` 文件，只需 **menuconfig → Example Configuration**。

### 3. `app_main()` 执行顺序

IDF 上电流程：bootloader → 启动 `app_main`。本章没有自建任务，整个闪烁逻辑就在这一个函数里循环。

**步骤 A · 复位引脚**

```c
ESP_ERROR_CHECK(gpio_reset_pin(BLINK_GPIO));
```

将该 GPIO 从其它外设（如有）上解开，恢复为可配置状态。换脚或换功能后建议总是先 `gpio_reset_pin`。

**步骤 B · 设为输出**

```c
ESP_ERROR_CHECK(gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT));
```

推挽输出，可直接驱动 LED（本板已有限流）。未调用 `gpio_set_level` 前，引脚电平通常为默认低。

**步骤 C · 死循环闪烁**

```c
while (1) {
    ESP_ERROR_CHECK(gpio_set_level(BLINK_GPIO, 1));
    vTaskDelay(BLINK_DELAY_MS / portTICK_PERIOD_MS);
    ESP_ERROR_CHECK(gpio_set_level(BLINK_GPIO, 0));
    vTaskDelay(BLINK_DELAY_MS / portTICK_PERIOD_MS);
}
```

| 语句 | 含义 |
|------|------|
| `gpio_set_level(..., 1)` | 输出高；本板 LED **高有效** |
| `gpio_set_level(..., 0)` | 输出低，LED 灭 |
| `vTaskDelay(n)` | 当前任务休眠 **n 个 tick**；常见 `CONFIG_FREERTOS_HZ=1000` 时 1 tick=1 ms，300 ms 即 `300/portTICK_PERIOD_MS` |

`while (1)` 不会把 CPU 占满：延时期间调度器会运行 **IDLE** 与其它任务。本章未使用 `ESP_LOGx`，因此串口无任何输出（04 章才正式用 USB 控制台）。

### 4. 与官方 `gpio` 例程的差异

改编自 `examples/peripherals/gpio` 的 blink：**删除日志**，只保留 GPIO 三步 + 循环；LED 引脚改为本板 **GPIO38**。

## 常见问题

- LED 不亮：确认 GPIO38、烧录成功、按 RST。
- 命令须带 `--preview`；路径勿含中文。

**下一章**：[02_GPIO_BUTTON.md](02_GPIO_BUTTON.md)