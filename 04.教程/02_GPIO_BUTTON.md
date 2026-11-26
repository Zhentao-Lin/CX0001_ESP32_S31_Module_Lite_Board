# 02 · GPIO 输入 - BOOT 按键

**示例工程**：[`03.Sketch/02_GPIO_BUTTON`](../03.Sketch/02_GPIO_BUTTON)

BOOT 键（GPIO61）下降沿中断 + 队列 + 任务内 5 ms 消抖，翻转 LED（GPIO38）。

> [00_开发环境.md](00_开发环境.md)。无串口输出。

## 本节目标

- `gpio_config` 输入、上拉、负边沿中断；
- `gpio_install_isr_service` / `gpio_isr_handler_add`；
- ISR 只投递队列；消抖在任务里 `vTaskDelay`。

## 用到的硬件

- BOOT（GPIO61）、LED（GPIO38）。

## 工程配置

1. 打开 `03.Sketch/02_GPIO_BUTTON`，`set-target esp32s31`。
2. menuconfig → **Example Configuration**：`BUTTON_GPIO`（61）、`LED_GPIO`（38）。

## 编译与下载

```powershell
idf.py --preview build
idf.py --preview -p <COM口> flash
```

VS Code：Build → Flash。观察 LED 与按键。

## 编程效果

- 上电 LED 灭；
- 每次有效按键 LED 翻转一次；
- 长按不连发。

## 代码逐段讲解

源码：[`main/gpio_button_main.c`](../03.Sketch/02_GPIO_BUTTON/main/gpio_button_main.c)。

本章在 01 章 GPIO 输出的基础上，加入 **输入、中断、FreeRTOS 队列、任务内消抖**。核心原则：**ISR 里只做最短路径通知，凡需要延时的逻辑放到任务里**。

### 1. 全局变量

```c
static QueueHandle_t s_press_queue;
static int s_led_level;
static int64_t s_last_press_us;
```

| 变量 | 作用 |
|------|------|
| `s_press_queue` | 深度 10 的队列，元素为 `uint32_t`，仅表示「发生过一次边沿」 |
| `s_led_level` | 软件记录 LED 亮灭；**不要**用 `gpio_get_level` 读自己刚写的输出脚（不可靠） |
| `s_last_press_us` | 上次**已确认有效**按键的时刻（`esp_timer_get_time()`，单位 µs），用于时间窗消抖 |

### 2. 中断服务函数 `button_isr_handler`

```c
static void IRAM_ATTR button_isr_handler(void *arg)
{
    uint32_t event = 1;
    xQueueSendFromISR(s_press_queue, &event, NULL);
}
```

- **`IRAM_ATTR`**：建议把 ISR 放在 IRAM，减少 flash cache miss 带来的中断延迟。
- **只投递队列**：不 `printf`、不 `vTaskDelay`、不翻转 LED。
- `xQueueSendFromISR`：从 ISR 向任务发数据的标准 API；第三个参数为 `NULL` 表示不请求立刻切换任务（也可传 `&xHigherPriorityTaskWoken` 在需要时 `portYIELD_FROM_ISR`）。

### 3. `app_main` 初始化 LED

与 01 章相同顺序，且 **默认灭灯**：

```c
gpio_reset_pin(LED_GPIO);
gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
gpio_set_level(LED_GPIO, 0);
```

### 4. 配置 BOOT 键为「上拉输入 + 下降沿中断」

```c
gpio_config_t io_conf = {
    .pin_bit_mask = 1ULL << BUTTON_GPIO,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_NEGEDGE,
};
gpio_config(&io_conf);
```

- 本板 BOOT **按下为低**，未按为内部上拉高。
- **`GPIO_INTR_NEGEDGE`**：只在 高→低 时进中断（一次按下一次边沿；长按不重复触发，除非有抖动产生多个下降沿）。

注册中断：

```c
gpio_install_isr_service(0);
gpio_isr_handler_add(BUTTON_GPIO, button_isr_handler, NULL);
```

`gpio_install_isr_service` 整芯片一次即可；`0` 表示默认中断标志。

### 5. 主循环：从队列取事件并消抖

```c
while (1) {
    if (xQueueReceive(s_press_queue, &event, portMAX_DELAY) == pdTRUE) {
```

阻塞等待 ISR 投递；无按键时任务休眠，不占 CPU。

**第一层消抖（时间戳）**

```c
if (esp_timer_get_time() - s_last_press_us < BUTTON_DEBOUNCE_MS * 1000) {
    continue;
}
```

距上次有效按下不足 **5 ms** 的边沿视为机械抖动附带边沿，丢弃。

**第二层消抖（延时 + 回读）**

```c
vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
if (gpio_get_level(BUTTON_GPIO) != 0) {
    continue;
}
```

任务里才能 `vTaskDelay`。延时后若键已松开（读回高），说明是毛刺而非真实按压。

**确认按下 → 翻转 LED**

```c
s_last_press_us = esp_timer_get_time();
s_led_level = !s_led_level;
gpio_set_level(LED_GPIO, s_led_level);
```

板上另有硬件消抖，软件 5 ms 是第二道防线，换键/换板仍可调节 `BUTTON_DEBOUNCE_MS`。

## 常见问题

- 无反应：GPIO61、复位退出下载模式。
- 多触发：增大 `BUTTON_DEBOUNCE_MS`。

**下一章**：[03_WS2812.md](03_WS2812.md)