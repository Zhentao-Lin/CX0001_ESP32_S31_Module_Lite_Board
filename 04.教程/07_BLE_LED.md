# 07 · BLE 控制 LED

**示例工程**：[`03.Sketch/07_BLE_LED`](../03.Sketch/07_BLE_LED)

自定义 GATT：写 `led_on` / `led_off` 控制 GPIO38；读/notify 状态。

> [00_开发环境.md](00_开发环境.md)。

## 本节目标

- 自定义 Service/Characteristic（0xFFE0 / 0xFFE1）；
- 写回调解析命令并驱动 GPIO；
- READ + WRITE + NOTIFY。

## 关键参数

| 项 | 值 |
|----|-----|
| 名 | `S31-BLE-LED` |
| 命令 | `led_on` / `led_off`（ASCII） |
| LED | GPIO38 高有效 |

## 工程配置

打开 `07_BLE_LED`，`set-target esp32s31`；蓝牙 sdkconfig 同 06。

## 编译与下载

```powershell
idf.py --preview build
idf.py --preview -p <COM口> flash monitor
```

nRF Connect → `0xFFE1` 写文本 `led_on` / `led_off`。

## 编程效果

写 `led_on` → LED 亮，notify `0x01`；`led_off` → 灭，`0x00`。

## 代码逐段讲解

源码：[`main/ble_led_main.c`](../03.Sketch/07_BLE_LED/main/ble_led_main.c)。

### 1. GATT `0xFFE0` / `0xFFE1`

`led_access_cb` 处理 **READ_CHR** / **WRITE_CHR**。内部状态 `s_led_state` 0/1 与 GPIO 一致。

### 2. `led_parse_command` / `apply_led_state`

严格长度 6/7 与字符串比较（大小写不敏感）。`apply_led_state`：`gpio_set_level(LED_GPIO, ...)`、打日志、若已连接则 `ble_gatts_notify_custom` 发 1 字节状态。

### 3. `led_access_cb` 分支

READ：`os_mbuf_append(&s_led_state,1)`。WRITE：flat 到 `payload` → parse → `apply_led_state` 或忽略未知命令。

### 4. `ble_advertise`

除设备名外可广播 **16 位 UUID**（`fields.uuids16`），手机扫描更易过滤。

### 5. `app_main`

`nvs` → `gpio_config` LED 输出默认灭 → `nimble_port_init` 与注册服务（同 06 骨架）→ **无** USB 控制台任务，仅 BLE 控灯。

## 常见问题

- 无动作：特征 0xFFE1、字符串命令、notify 订阅。

**下一章**：[08_WIFI_SOCKET.md](08_WIFI_SOCKET.md)