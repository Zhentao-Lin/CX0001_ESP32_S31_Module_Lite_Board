# 04 · USB 串口（USB-Serial-JTAG）

**示例工程**：[`03.Sketch/04_USB_SERIAL`](../03.Sketch/04_USB_SERIAL)

一根 USB-C 完成烧录、日志与命令行：`help` / `led on|off` / `info` / `echo`。

> [00_开发环境.md](00_开发环境.md)。使用 **USB-Serial-JTAG 口**（非 USB-OTG）。

## 本节目标

- `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`；
- 须安装 `usb_serial_jtag` 驱动并 `vfs_use_driver`，`select()` 才能等待输入；
- 按行解析命令，避免看门狗空转。

## 用到的硬件

- USB-Serial-JTAG、LED GPIO38。

## 工程配置

1. 打开 `03.Sketch/04_USB_SERIAL`，`set-target esp32s31`。
2. `sdkconfig.defaults` 已设 USB 控制台。
3. menuconfig → **Example Configuration** → `LED_GPIO`（38）。

## 编译与下载

```powershell
idf.py --preview build
idf.py --preview -p <COM口> flash monitor
```

须在 **Monitor** 里交互。

## 编程效果

```
=== 04 USB-Serial: on-board USB CDC console ===
> info
chip        : ESP32-S31 rev 0
...
> led on
OK: LED on
```

`led on` / `led off` 立即控制 LED。

## 代码逐段讲解

源码：[`main/usb_serial_main.c`](../03.Sketch/04_USB_SERIAL/main/usb_serial_main.c)。

### 1. 全局状态

```c
static char s_line[LINE_MAX_LEN];   // 当前正在编辑的一行，最长 80
static size_t s_line_len;
static bool s_led_on;
```

`s_led_on` 与 GPIO 同步，供 `info` 打印；`LINE_MAX_LEN` 超长会整行丢弃，避免执行半截命令。

### 2. `app_main`：USB 驱动 + VFS（关键）

`sdkconfig` 里 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` 只把 **stdin/stdout 注册到 USB 口**；**不等于**装了底层驱动。

```c
usb_serial_jtag_driver_install(&driver_config);
usb_serial_jtag_vfs_use_driver();
```

若不安装驱动，`select(stdin)` 会因 `ESP_ERR_INVALID_STATE` 立刻返回 **-1**，主循环空转 → **IDLE 饿死 → Task WDT**。这是 04/08 章共通踩坑点。

随后初始化 LED 为输出、默认灭，并 `printf` 横幅与 `help`。

### 3. 辅助函数

- **`print_help` / `print_info`**：`info` 读 `esp_chip_info`、`esp_flash_get_size`、`esp_psram_get_size`、`esp_get_free_heap_size`、`usb_serial_jtag_is_connected()` 等。
- **`trim_line`**：去掉行尾 `\r`/`\n`/空格，兼容 CRLF。
- **`handle_line`**：对整行做 `strcmp`/`strncmp` 分发 `help` | `info` | `led on/off` | `echo ` | 未知命令报错。

### 4. 主循环：事件等待 + 行编辑

```c
select(STDIN_FILENO + 1, &read_set, NULL, NULL, &timeout);  // 100 ms
read(STDIN_FILENO, chunk, READ_CHUNK_LEN);
```

- **`ready < 0`**：`errno` 多为 EINTR，退避 200 ms。
- **`ready == 0`**：超时，无输入，继续轮询。
- 逐字节处理：`\\r` 忽略，`\\n` 结束一行并 `handle_line`；**退格**在设备端回显（IDF 6.x monitor 不再本地回显按键）。

### 5. 命令表

| 命令 | 代码路径 |
|------|----------|
| `help` | `print_help()` |
| `led on` / `off` | `gpio_set_level(LED_GPIO,1/0)` + `s_led_on` |
| `info` | `print_info()` |
| `echo xxx` | `printf("echo: %s", line+5)` |

## 常见问题

- 无输出：控制台通道为 USB Serial/JTAG；插对口。
- 能看不能敲：未装 USB 驱动 → WDT；见代码逐段 1。
- PuTTY 双显：关闭本地回显。

**下一章**：[05_USB_MSC.md](05_USB_MSC.md)