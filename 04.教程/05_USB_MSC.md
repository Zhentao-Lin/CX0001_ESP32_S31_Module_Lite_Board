# 05 · 虚拟 U 盘（USB-OTG + Flash）

**示例工程**：[`03.Sketch/05_USB_MSC`](../03.Sketch/05_USB_MSC)

TinyUSB MSC 将 Flash `fatfs` 分区暴露为 U 盘；PC 格式化为 FAT32 后读写。

> [00_开发环境.md](00_开发环境.md)。**烧录口 Serial-JTAG，U 盘口 USB-OTG**。

## 本节目标

- USB Device MSC；块大小 4096；FS 由主机维护；
- `wl_mount` + `tinyusb_msc_new_storage_spiflash`；
- `MOUNT_USB`：上电即交给主机。

## 用到的硬件

- Flash `fatfs` 分区、USB-OTG 线。

## 介质参数

| 项 | 值 |
|----|-----|
| 逻辑块 | 4096 字节 |
| 块数 | 约 2506（≈9.79 MiB 可用） |
| 组件 | `espressif/esp_tinyusb` |

容量 2506 块 = 分区扇区 − WL 元数据（约 22 扇区），属正常。

## 工程配置

1. 打开 `05_USB_MSC`，`set-target esp32s31`。
2. 首次 build 拉取 `esp_tinyusb`（联网）。
3. `partitions_16M.csv` 与 `CONFIG_PARTITION_TABLE_OFFSET=0xA000` 一致。
4. menuconfig：TinyUSB **Mass Storage** 启用。

## 编译与下载

```powershell
idf.py --preview build
idf.py --preview -p <COM口> flash monitor
```

Monitor 接 Serial-JTAG；U 盘接 OTG → 日志 `USB MSC ready...` → PC 格式化 FAT32。

## 编程效果

1. `Initialising wear levelling...`  
2. 电脑出现可移动磁盘；挂载日志 `Storage mounted to the USB host`；  
3. 周期状态 `owned by the USB host | 2506 blocks`；  
4. 安全弹出后掉电再上电，文件仍在。

## 代码逐段讲解

源码：[`main/usb_msc_main.c`](../03.Sketch/05_USB_MSC/main/usb_msc_main.c)。

### 1. USB 描述符（`s_device_desc` / `s_msc_fs_config`）

向 PC 声明：Espressif VID、产品名 **S31 Flash MSC**、**单一 MSC 接口**、Bulk 端点 0x01/0x81、包长 64（全速）。设备类为 MISC + IAD，符合 TinyUSB 组合设备惯例。

### 2. `storage_init_spiflash(wl_handle)`

```c
esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
wl_mount(data_partition, wl_handle);
```

在 `partitions_16M.csv` 里找 **FAT 数据分区**（标签常为 `fatfs`）。`wl_mount` 打开磨损均衡层，对上层表现为固定大小扇区（本板 **4096 字节/扇区**），主机 SCSI 读写的 LBA 经 WL 映射到真实 Flash。

### 3. `storage_event_cb`

响应 `MOUNT_START` / `MOUNT_COMPLETE` / `MOUNT_FAILED` / `FORMAT_REQUIRED` 等。未格式化时 PC 会提示格式化；**本示例不在设备端格式化**，必须由用户在 PC 上选 **FAT32**。

### 4. `app_main` 初始化顺序（顺序不能乱）

1. **`tinyusb_msc_install_driver`**：`auto_mount_off = 1` —— 主机弹出后不要让介质自动回到应用侧，否则 Windows 显示「请插入磁盘」。
2. **`storage_cfg.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB`**：上电即交给主机；`do_not_format = true` 防止误伤主机已写的 FAT。
3. **`tinyusb_msc_new_storage_spiflash`**：绑定 `wl_handle`。
4. **`tinyusb_driver_install`**：挂上自定义描述符，USB-OTG 口枚举为 U 盘。

应用**不**在主机挂载期间读写该分区，避免破坏 FAT。

### 5. 主循环状态报告

每 5 s 查询 `tinyusb_msc_get_storage_mount_point` / `get_storage_capacity`，打印 `owned by the USB host | N blocks`（N 多为 **2506**）。仅归属或块数变化时打日志，避免刷屏。

## 常见问题

- 无盘：OTG 口、数据线、MSC 使能。
- 「请插入磁盘」：在 PC 格式化为 FAT32。
- 拔线前安全弹出。

**下一章**：[06_BLE_UART.md](06_BLE_UART.md)