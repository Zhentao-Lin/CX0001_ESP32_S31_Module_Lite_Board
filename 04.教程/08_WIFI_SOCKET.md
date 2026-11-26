# 08 · Wi-Fi 数据透传（TCP / UDP）

**示例工程**：[`03.Sketch/08_WIFI_SOCKET`](../03.Sketch/08_WIFI_SOCKET)

menuconfig **三选一**：TCP server / TCP client / UDP echo。`idf.py monitor` 窗口可敲一行发给对端。

> [00_开发环境.md](00_开发环境.md)。片上 `esp_wifi`；仅 **2.4 GHz**。

## 本节目标

- TCP：`bind`/`listen`/`accept` 或 `connect` 重试；
- UDP：`recvfrom`/`sendto`、数据报边界；
- 控制台 + socket **多任务**全双工；USB 串口驱动同第 04 章。

## 用到的硬件

- 2.4G 路由器、同网段 PC、USB-Serial-JTAG。

## 环境与 menuconfig

- `idf_component.yml` 无 `esp_hosted` / `esp_wifi_remote`。
- **Wi-Fi station example**：SSID、密码。
- **Socket example**：模式、端口（UDP 监听 **3334** 与广播 **3333** 勿相同）。

默认端口：TCP 3333；UDP listen 3334 / broadcast 3333。

## 工程配置

1. 打开 `03.Sketch/08_WIFI_SOCKET`，`set-target esp32s31`。
2. `idf.py --preview menuconfig` 填 Wi-Fi 与 Socket 模式。
3. 改模式后须重新 **build**。

## 编译与下载

```powershell
idf.py --preview build
idf.py --preview -p <COM口> flash monitor
```

记下日志 **`Got IP: <板子IP>`**。

### 对端联调（`<ip>` 为板子 IP）

```bash
# TCP server：电脑连板子
nc <ip> 3333
```

```bash
# TCP client：电脑先监听
nc -l 3333
```

```python
# server.py — 板子 client 模式连到本机 3333
import socket
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("", 3333)); s.listen(1)
c, addr = s.accept(); print("board from", addr)
while True:
    d = c.recv(256)
    if not d: break
    print("<-", d.decode(errors="replace"))
    c.sendall(input("-> ").encode() + b"\n")
```

```bash
# UDP 单包回显测试
python -c "import socket;s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.settimeout(3);s.sendto(b'ping',('<ip>',3334));print(s.recvfrom(1024))"
```

## 编程效果

**TCP server** 典型日志：

```
I (o) wifi_socket: Got IP: 192.168.1.123
I (o) wifi_socket: TCP server listening on port 3333
I (o) wifi_socket: TCP peer connected from 192.168.1.50:52311
I (o) wifi_socket: TCP RX 5 bytes: hello
I (o) wifi_socket: Console -> peer (5 bytes): world
```

**UDP**：`UDP RX ... ping` 与 `Console -> broadcast :3333`。

monitor 里输入一行回车即发；收到内容以 `TCP RX` / `UDP RX` 一行打出。

## 代码逐段讲解

源码：[`main/wifi_socket_main.c`](../03.Sketch/08_WIFI_SOCKET/main/wifi_socket_main.c)。**编译期**由 menuconfig 三选一，未选中的 `#if` 块不会进固件。

### 1. Wi-Fi：`wifi_event_handler` + `wifi_start`

- `STA_START` → `esp_wifi_connect()`。
- `STA_DISCONNECTED` → 按 `CONFIG_EXAMPLE_WIFI_MAX_RETRY` 重连或置 `WIFI_FAIL_BIT`（日志带 **reason**）。
- `GOT_IP` → 打印 IP/掩码/网关，置 `WIFI_CONNECTED_BIT`。
- `wifi_start()` 创建默认 STA netif，填 SSID/密码（空密码则 `WIFI_AUTH_OPEN`），`EventGroupWaitBits` 等到连上或放弃。

### 2. 控制台 → 发送队列：`console_input_task`

与 04 章类似：**阻塞 `read(stdin)`**，不用 `select`（本文件包含 `lwip/sockets.h` 时 `select` 会变成 **`lwip_select`**，不认 stdin）。成行后 `xQueueSend(s_tx_queue, line, 0)`；队列深 **8**，满则丢行。设备端回显按键。

### 3. TCP 发送：`sock_tx_task` + `line_send`

独立任务因为 **`recv` 在另一任务阻塞**。`s_link_sock == -1` 时丢弃控制台行。`line_send` 循环 `send` 直至发完，再补发 `\\n` 方便对端 `nc` 显示。

### 4. TCP 接收：`tcp_rx_session` / `tcp_rx_publish`

`recv` 收到数据 → `to_printable`（非打印变 `.`，去掉 CR/LF）→ 打一条 `TCP RX`。`tcp_rx_publish` 在会话期间把 `s_link_sock` 交给发送任务。

### 5. TCP server 模式 `tcp_server_task`

`socket` → `SO_REUSEADDR` → `bind INADDR_ANY:TCP_PORT` → `listen(1)` → 循环 `accept` → `tcp_rx_publish(client)` → `close`。同一时刻只服务一个客户端。

### 6. TCP client 模式 `tcp_client_task`

`inet_pton` 解析 **点分十进制** `REMOTE_IP`（不支持主机名）；`connect` 失败每 **2 s** 重试；断链后同样 2 s 再连。

### 7. UDP 模式 `udp_bridge_task`

- `bind` 监听 `UDP_PORT`；`SO_BROADCAST` 允许发往 `255.255.255.255:BCAST_PORT`。
- `select` 100 ms：可读则 `recvfrom` → 日志 → **单播** `sendto` 回发送方（回显）。
- 否则 Drain `s_tx_queue`，每行一个 **广播** 报文（含 `\\n` 在 payload 内，避免拆成两个报文）。
- **监听端口与广播端口必须不同**（默认 3334 / 3333），否则自激。

### 8. `app_main` 任务拓扑

`wifi_start` → USB 驱动 + VFS → `s_tx_queue` → `console_input_task`(prio 5) → UDP 时单任务 `udp_bridge`(6144 栈) **或** TCP 时 `sock_tx`(4) + `tcp_server/client`(4)。

## 常见问题

- 键盘无反应：USB Serial/JTAG 驱动 + VFS（同 04 章）。
- client 连不上：IP/端口、防火墙、对端未 listen。
- UDP 刷屏：两端口配成相同。
- Wi-Fi 失败：2.4G、SSID/密码、日志 `reason` 码。

**下一章**：[09_WEB_WS2812.md](09_WEB_WS2812.md)