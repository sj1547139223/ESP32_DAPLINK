# DAPLink WiFi 无线调试指南

支持两种模式：局域网直连（零配置）和跨网络调试（通过中继服务器）。

## 版本概览

| 组件 | 版本 | 说明 | 版本号位置 |
|------|------|------|-----------|
| **ESP32-S3 固件** | v1.0.0 | CMSIS-DAP v2 调试器 + WiFi + USB MSC | `esp32s3-idf/src/main.cpp` → `kVersion` |
| **PC 工具** | v1.0.0 | 串口终端 + 协议收发 + WiFi桥接 | `pc-tools/daplink_tool.py` → `APP_VERSION` |
| **PC 桥接** | v1.0.0 | 跨网络桥接客户端 | `pc-bridge/bridge.py` 文件头 |
| **中继服务器** | v1.0.0 | WebSocket 跨网络中继 | `relay-server/package.json` → `version` |

## 架构概览

```
方式1: 同网络直连 (局域网，零额外部署)
┌─────────┐        ┌──────────────┐        ┌───────────┐
│  Keil   │◄─TCP──►│  ESP32-S3    │◄─SWD──►│ 目标 MCU  │
│(elaLink)│  3240  │  DAPLink     │        │           │
└─────────┘        └──────────────┘        └───────────┘

方式2: 跨网络 (通过中继服务器，支持不同局域网/不同城市)
┌─────────┐     ┌──────────┐     ┌──────────┐     ┌──────────────┐     ┌───────────┐
│  Keil   │◄───►│ PC Bridge│◄───►│  Relay   │◄───►│  ESP32-S3    │◄───►│ 目标 MCU  │
│(elaLink)│TCP  │ (bridge) │ WS  │  Server  │ WS  │  DAPLink     │SWD  │           │
└─────────┘     └──────────┘     └──────────┘     └──────────────┘     └───────────┘
    本地          本地PC           云服务器          远程现场            远程现场
```

---

## 方式1: 局域网直连（最简单）

> **适用场景**：PC 和 DAPLink 在同一 WiFi/局域网中

### 1.1 你需要准备

| 项目 | 说明 |
|------|------|
| ESP32-S3 DAPLink | 已烧录本项目固件 |
| elaphureLink | Keil 专用 WiFi 调试驱动（免费工具）|
| SWD 连线 | DAPLink 已通过 SWD 连接目标 MCU |

### 1.2 配置 ESP32 WiFi

1. 将 ESP32 DAPLink 通过 USB 插入电脑
2. 电脑上会出现一个 **U 盘**（名为 `DAP_MSC`）
3. 在 U 盘根目录创建文件 `wifi.txt`，内容如下：

```
ssid=你的WiFi名称
pass=你的WiFi密码
```

4. **拔插 ESP32 重启**（或按复位键）
5. ESP32 启动后自动连接 WiFi，通过串口监视器可看到打印的 **IP 地址**，例如：

```
wifi_debug: WiFi connected! IP: 192.168.3.24, elaphureLink port: 3240
```

> **记住这个 IP 地址**，后面填入 elaphureLink 要用。

### 1.3 安装 elaphureLink（Keil 端，仅首次）

1. 从 GitHub 下载 elaphureLink：  
   https://github.com/windowsair/elaphureLink/releases  
   下载最新的 `elaphureLink-vX.X.X.zip`

2. 解压到任意目录

3. **运行** `elaphureLink.Wpf.exe`

4. 点击 **Install Driver** 按钮（仅首次安装需要）  
   这会将 `elaphureLink.dll` 注册到 Keil，安装完成后 Keil 调试器选项中会多出 `elaphureLink CMSIS-DAP`

### 1.4 连接调试

1. **打开** `elaphureLink.Wpf.exe`
2. 在 **IP Address** 框中输入 ESP32 的 IP 地址，例如 `192.168.3.24`
3. 端口保持默认 `3240`
4. 点击 **Connect** 按钮，显示 `Connected` 即成功

5. **打开 Keil 项目**：
   - 菜单 → Options for Target → Debug 标签
   - 右侧选择 **elaphureLink CMSIS-DAP**
   - 点击 Settings → 可以看到识别到的调试器
   - 确认 SW Device 中识别到目标 MCU 的 DPIDR

6. **开始调试**：点击 Keil 的 Download 下载程序，或点击 Debug 开始仿真

---

## 方式2: 跨网络调试（通过中继服务器）

> **适用场景**：PC 和 DAPLink 不在同一网络（如远程设备调试、不同办公地点等）

### 整体流程

```
第一步：在服务器上部署中继服务        → 一条命令完成
第二步：配置 ESP32 连接中继服务器      → 在 wifi.txt 加一行
第三步：在 PC 上运行桥接工具          → 双击 start.bat
第四步：在 Keil 中连接调试            → 和局域网一样的操作
```

### 2.1 部署中继服务器

> 需要一台有公网 IP 的服务器（VPS），推荐 Linux 系统。

#### 方法 A：Docker 一键部署（推荐）

将 `relay-server/` 文件夹上传到服务器后执行：

```bash
cd relay-server
bash install.sh
```

脚本会自动：
- 安装 Docker（如未安装）
- 构建并启动中继服务
- 打印中继服务器地址

部署完成后显示：

```
中继服务器地址: ws://你的服务器IP:7000
```

**管理命令：**

```bash
cd relay-server
docker compose logs -f       # 查看实时日志
docker compose down          # 停止服务
docker compose restart       # 重启服务
docker compose up -d --build # 更新并重启
```

#### 方法 B：直接运行（需要 Node.js）

```bash
# 安装 Node.js (如未安装)
curl -fsSL https://deb.nodesource.com/setup_18.x | sudo -E bash -
sudo apt-get install -y nodejs

# 部署
cd relay-server
npm install
node index.js
```

如需后台运行：

```bash
# 使用 nohup
nohup node index.js > relay.log 2>&1 &

# 或使用 pm2 (推荐)
npm install -g pm2
pm2 start index.js --name daplink-relay
pm2 save
pm2 startup    # 设置开机自启
```

#### 防火墙配置

确保服务器的 **7000 端口** 已开放：

```bash
# Ubuntu/Debian
sudo ufw allow 7000/tcp

# CentOS
sudo firewall-cmd --permanent --add-port=7000/tcp
sudo firewall-cmd --reload

# 云服务器还需在控制台的安全组中放行 7000 端口
```

### 2.2 配置 ESP32

在 U 盘 `wifi.txt` 中添加第三行 `relay=`：

```
ssid=现场的WiFi名称
pass=现场的WiFi密码
relay=ws://你的服务器IP:7000
```

ESP32 重启后，串口输出会打印：
```
wifi_debug: WiFi connected! IP: 192.168.x.x, elaphureLink port: 3240
wifi_debug: Device pairing code: A3K7N2
wifi_debug: WS relay client started: ws://你的服务器IP:7000
```

> **记住 6 位配对码**（如 `A3K7N2`），PC 端桥接工具需要输入。

### 2.3 PC 端运行桥接工具

#### Windows 用户

1. 确保已安装 [Python](https://www.python.org/downloads/)（安装时勾选 **Add Python to PATH**）
2. 双击 `pc-bridge\start.bat`
3. 按提示输入：
   - 中继服务器地址，例如 `ws://123.45.67.89:7000`
   - 设备配对码，例如 `A3K7N2`

```
╔══════════════════════════════════╗
║   DAPLink 无线调试桥接工具       ║
╚══════════════════════════════════╝

请输入中继服务器地址 (如 ws://192.168.1.100:7000): ws://123.45.67.89:7000
请输入设备配对码 (6位): A3K7N2
正在连接中继服务器: ws://123.45.67.89:7000
已注册，配对码: A3K7N2，等待设备连接...
✓ 设备已连接！

=== 桥接就绪 ===
本地 TCP 端口: 127.0.0.1:3240
请在 elaphureLink 中连接到: 127.0.0.1:3240
按 Ctrl+C 退出
```

也可以使用命令行参数跳过交互：

```cmd
python bridge.py --relay ws://123.45.67.89:7000 --code A3K7N2
```

### 2.4 Keil 连接调试

1. 打开 `elaphureLink.Wpf.exe`
2. IP 地址输入 `127.0.0.1`，端口 `3240`
3. 点击 Connect
4. 在 Keil 中正常调试即可

---

## wifi.txt 配置参考

在 ESP32 U 盘根目录创建 `wifi.txt` 文件：

```
# WiFi 配置（必填）
ssid=MyWiFiNetwork
pass=MyPassword123

# 中继服务器（可选，仅跨网络时填写）
relay=ws://relay.example.com:7000
```

| 参数 | 必填 | 说明 |
|------|------|------|
| `ssid=` | ✅ | WiFi 名称（SSID） |
| `pass=` | ❌ | WiFi 密码，开放网络可不填 |
| `relay=` | ❌ | 中继服务器 WebSocket URL，跨网络时填写 |

- `#` 开头的行为注释，会被忽略
- 文件编码：UTF-8 或 ASCII
- 修改后需重启 ESP32 生效

---

## 项目结构

```
daplink/
├── esp32s3-idf/          # ESP32-S3 固件源码（已集成 WiFi 调试）
│   └── src/
│       ├── wifi_debug.cpp  # WiFi 调试核心代码
│       └── wifi_debug.h
├── relay-server/          # 中继服务器（Node.js）
│   ├── index.js            # 服务端代码
│   ├── package.json        # 依赖配置
│   ├── Dockerfile          # Docker 镜像
│   ├── docker-compose.yml  # Docker Compose 配置
│   └── install.sh          # 一键部署脚本
├── pc-bridge/             # PC 端桥接工具（Python）
│   ├── bridge.py           # 桥接程序
│   ├── requirements.txt    # Python 依赖
│   └── start.bat           # Windows 一键启动
└── README_WIFI.md         # 本文档
```

---

## 常见问题

### ESP32 连不上 WiFi
- 检查 `wifi.txt` 中 SSID 和密码是否正确（注意大小写）
- 确认 WiFi 是 2.4GHz 频段（ESP32 不支持 5GHz）
- 通过串口监视器查看错误信息

### elaphureLink 连接超时
- 确认 ESP32 的 IP 地址正确（串口打印的 IP）
- 检查 PC 和 ESP32 是否在同一子网
- 确认端口 3240 未被防火墙阻止

### 跨网络连接失败
- 检查中继服务器是否正常运行：`docker compose logs`
- 确认服务器 7000 端口已开放（防火墙 + 安全组）
- 确认 ESP32 的 `wifi.txt` 中 `relay=` 地址正确
- 确认 PC 桥接工具输入的配对码与 ESP32 串口打印的一致

### Keil 提示找不到调试器
- 确认已运行 `elaphureLink.Wpf.exe` 并 Install Driver
- 在 Keil → Options → Debug → 右侧选择 **elaphureLink CMSIS-DAP**

---

## 技术细节

- 基于 [elaphureLink](https://github.com/windowsair/elaphureLink) 协议与 Keil 通信
- TCP 直连：ESP32 运行 elaphureLink 兼容的 TCP 服务器（端口 3240）
- 跨网络：WebSocket 中继 + PC 本地 TCP 代理，通过配对码匹配设备
- CMSIS-DAP 命令在 ESP32 本地处理，支持 SWD 调试全部功能
- WiFi 调试与 USB 调试可同时工作，互不影响

---

## 致谢 & 引用

本项目的开发离不开以下开源项目和工具，在此表示衷心感谢：

| 项目 | 说明 | 链接 |
|------|------|------|
| **elaphureLink** | 无线 CMSIS-DAP 调试方案，本项目 WiFi 调试功能的协议基础 | [GitHub](https://github.com/windowsair/elaphureLink) |
| **TinyUSB** | 轻量级 USB 设备协议栈，用于实现 CMSIS-DAP v2、CDC 串口及 MSC 存储 | [GitHub](https://github.com/hathach/tinyusb) |
| **ESP-IDF** | 乐鑫官方 ESP32 开发框架 | [GitHub](https://github.com/espressif/esp-idf) |
| **CMSIS-DAP** | ARM 官方调试接口规范 | [GitHub](https://github.com/ARM-software/CMSIS_5) |
| **OpenOCD** | 开源调试工具，兼容 CMSIS-DAP | [官网](https://openocd.org/) |
| **PlatformIO** | 嵌入式开发构建工具 | [官网](https://platformio.org/) |
| **ws** | Node.js WebSocket 库，用于中继服务器 | [GitHub](https://github.com/websockets/ws) |

特别感谢 [windowsair](https://github.com/windowsair) 的 **elaphureLink** 项目，为 WiFi 无线调试提供了协议规范和 Keil 驱动支持。

---

## 许可证

本项目仅供学习和个人使用。其中引用的第三方组件请遵循各自的许可协议。
本项目由AI编写。
