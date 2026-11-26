# 06 · BLE 数据透传（NUS）

**示例工程**：[`03.Sketch/06_BLE_UART`](../03.Sketch/06_BLE_UART)

Nordic UART Service：写 RX → 板子 notify TX 回显；串口打印 payload。

> [00_开发环境.md](00_开发环境.md)。片上 NimBLE，无 `esp_hosted`。

## 本节目标

- GATT 服务/特征；WRITE 与 NOTIFY；
- NUS 128 位 UUID；UUID 放 scan response；
- `nimble_port_init()` 起片上控制器。

## 用到的硬件

- 手机 + nRF Connect 等。

## 关键参数

| 项 | 值 |
|----|-----|
| 名 | `S31-BLE-UART` |
| 服务 | `6E400001-...-CCA9E` |
| RX / TX | `...0002...` / `...0003...` |

`sdkconfig.defaults`：`BT_ENABLED`、`BT_NIMBLE_ENABLED`、`BT_CONTROLLER_ENABLED`。

## 工程配置

1. 打开 `06_BLE_UART`，`set-target esp32s31`。
2. 一般无需 menuconfig 改蓝牙。

## 编译与下载

```powershell
idf.py --preview build
idf.py --preview -p <COM口> flash monitor
```

手机：连接 `S31-BLE-UART`，RX 写入，TX 开 notify。

## 编程效果

1. `Advertising as "S31-BLE-UART"`  
2. 连接、`Subscribe ... notify=1`  
3. 写 `hello` → 手机与串口均见回显  
4. 断开后再广播

## 代码逐段讲解

源码：[`main/ble_uart_main.c`](../03.Sketch/06_BLE_UART/main/ble_uart_main.c)。

### 1. GATT 表 `s_gatt_svcs[]`

NUS 三条 128 位 UUID（`BLE_UUID128_INIT` 小端字节序）。TX 特征：`BLE_GATT_CHR_F_NOTIFY`，注册后得到 `s_tx_val_handle`。RX 特征：`WRITE | WRITE_NO_RSP`，手机写入触发 `nus_access_cb`。

### 2. `nus_access_cb`

仅处理 `BLE_GATT_ACCESS_OP_WRITE_CHR`：`ble_hs_mbuf_to_flat` 拷到 `payload` → `log_payload`（`68 65 6C... | hello`）→ `ble_hs_mbuf_from_flat` + **`ble_gatts_notify_custom(conn, s_tx_val_handle, om)`** 回显。手机须在 TX 上写 CCCD 订阅 notify。

### 3. `ble_send_to_phone` / `console_input_task`

`ble_send_to_phone` 检查 `s_conn_handle`，组 mbuf 后 notify。`console_input_task` 逻辑同 04 章 USB 行编辑，一行发送到手机（双向透传）。

### 4. `ble_advertise` / `gap_event_cb`

主广播区放设备名；**scan response** 放 128 位 NUS UUID（否则 31 字节溢出）。`gap_event_cb` 维护连接句柄、断开重广播、`SUBSCRIBE` 时打 `notify=1` 日志。

### 5. `app_main`

`nvs_flash_init` → **`nimble_port_init()`**（ESP32-S31 片上 BT，勿走 esp_hosted）→ `ble_gatts_count_cfg` / `add_svcs` → `nimble_port_freertos_init(ble_host_task)` 跑 `nimble_port_run()` → USB 驱动 + `xTaskCreate(console_input_task, ..., 5, ...)`。

## 常见问题

- 搜不到：`BT_CONTROLLER_ENABLED`；看 Advertising 日志。
- 无 notify：手机开 CCCD。
- MTU 23：长包拆分。
- `ble_transport_to_ll_*`：删旧 `sdkconfig` 重 `set-target`。

**下一章**：[07_BLE_LED.md](07_BLE_LED.md)