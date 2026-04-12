# DAPLink PC Bridge v1.0.0

PC 端桥接工具，配合中继服务器实现跨网络无线调试。支持 WebSocket 和 TCP 两种中继协议。

## 版本历史

| 版本 | 修改内容 |
|------|----------|
| **v1.0.0** | 初始版本。WebSocket/TCP双协议中继、本地TCP服务器(DAP:3240/串口:3241/RTT:3242)、配对码机制、config.ini配置持久化 |

> 版本号定义在 `bridge.py` 文件头docstring中。

## 快速启动（Windows）

双击 `start.bat`，首次运行按提示输入中继服务器地址和配对码。
输入的配置会自动保存到 `config.ini`，下次启动自动加载，无需重复输入。

### 配置文件 config.ini

```ini
[bridge]
relay = ws://your-server:7000
code = A3K7N2
port = 3240
serial_port = 3241
rtt_port = 3242
protocol = ws
relay_tcp_port = 7001
```

- `relay` — 中继服务器地址
- `code` — 设备配对码 (6位)
- `port` — 本地 DAP TCP 端口 (默认 3240)
- `serial_port` — 本地串口 TCP 端口 (默认 3241)
- `rtt_port` — 本地 RTT TCP 端口 (默认 3242)
- `protocol` — 中继协议: `ws` (WebSocket) 或 `tcp` (TCP)
- `relay_tcp_port` — TCP 中继端口 (默认 7001)

留空的字段会在运行时交互式提示输入。命令行参数优先级最高。

## 手动运行

```bash
pip install -r requirements.txt

# 交互式 (WebSocket 模式)
python bridge.py

# WebSocket 模式 (命令行参数)
python bridge.py --relay ws://your-server:7000 --code A3K7N2

# TCP 模式
python bridge.py --relay your-server --code A3K7N2 --tcp

# TCP 模式 (自定义端口)
python bridge.py --relay your-server --code A3K7N2 --tcp --relay-tcp-port 7001
```

### 两种中继协议对比

| 特性 | WebSocket (默认) | TCP |
|------|:-:|:-:|
| 中继端口 | 7000 | 7001 |
| 数据帧格式 | WebSocket 帧 | 2字节长度前缀 |
| 防火墙友好 | ✅ (HTTP 升级) | 一般 |
| 开销 | 稍高 | 更低 |

## 打包为 exe（无需 Python 环境）

```bash
pip install pyinstaller
pyinstaller --onefile bridge.py
```

生成的 `dist/bridge.exe` 可直接分发使用。

## 搭配 elaphureLink 使用

1. 下载 [elaphureLink](https://github.com/windowsair/elaphureLink/releases) (免安装，解压即用)
2. 运行 `elaphureLink.Wpf.exe`，点击 "Install Driver" 安装 Keil 驱动 (仅首次需要)
3. 运行本桥接工具，输入中继服务器地址和配对码
4. 在 Keil 中选择 elaphureLink 调试器
5. 设置 IP 为 `127.0.0.1`，端口 `3240`
6. 开始调试！

## 同网络直连 (无需本工具)

如果 PC 和 DAPLink 在同一局域网内，可直接使用 elaphureLink 连接：
1. 在 ESP32 的 USB 盘中创建 `wifi.txt` 文件（不需要配置 relay）
2. ESP32 连接 WiFi 后，串口会打印 IP 地址
3. 在 elaphureLink 中直接输入 ESP32 的 IP 和端口 3240
