# ESP32-S3 DAPLink v1.0.0 — CMSIS-DAP v2 调试器

基于 ESP32-S3 的全功能 CMSIS-DAP v2 调试器，支持 USB 有线调试（Keil/IAR/OpenOCD）、WiFi 无线调试、USB CDC 串口桥接、USB MSC 拖拽烧录。

## 版本历史

| 版本 | 修改内容 |
|------|----------|
| **v1.0.0** | 初始版本。CMSIS-DAP v2(WinUSB)、SWD调试、WiFi无线调试(直连+中继)、USB CDC双串口桥接、USB MSC拖拽烧录(HEX/FLM)、RTT读取、LED状态指示 |

> 版本号定义在 `src/main.cpp` → `kVersion` 常量，启动时通过串口打印。

---

## 目录

- [1. 硬件连接](#1-硬件连接)
- [2. 编译与烧录](#2-编译与烧录)
- [3. USB 功能说明](#3-usb-功能说明)
  - [3.1 CMSIS-DAP v2（SWD 调试）](#31-cmsis-dap-v2swd-调试)
  - [3.2 CDC 虚拟串口](#32-cdc-虚拟串口)
  - [3.3 MSC 拖拽烧录](#33-msc-拖拽烧录)
- [4. WiFi 无线调试](#4-wifi-无线调试)
- [5. LED 指示灯](#5-led-指示灯)
- [6. FLM 闪存算法](#6-flm-闪存算法)
- [7. 支持的目标芯片](#7-支持的目标芯片)
- [8. PC 工具](#8-pc-工具)
  - [8.1 PC Bridge（跨网桥接）](#81-pc-bridge跨网桥接)
  - [8.2 DAPLink Tool（综合工具）](#82-daplink-tool综合工具)
- [9. 中继服务器](#9-中继服务器)
- [10. 技术细节](#10-技术细节)
- [11. 已知限制](#11-已知限制)

---

## 1. 硬件连接

### 1.1 GPIO 引脚分配

| 功能 | GPIO | 方向 | 说明 |
|------|------|------|------|
| **SWCLK** | 7 | 输出 | SWD 时钟线 |
| **SWDIO (In)** | 5 | 输入 | SWD 数据线输入（板载上拉） |
| **SWDIO (Out)** | 6 | 输出 | SWD 数据线输出 |
| **nRST** | 3 | 开漏 | 目标复位（低有效，板载上拉） |
| **LED_R** | 38 | 输出 | 红色状态 LED |
| **LED_G** | 21 | 输出 | 绿色状态 LED |
| **UART_TX** | 17 | 输出 | 串口桥接发送 |
| **UART_RX** | 18 | 输入 | 串口桥接接收 |

> **注意**：GPIO33-37 被 Octal PSRAM 占用，不可另作他用。

### 1.2 接线示意

```
ESP32-S3 DAPLink          目标 MCU
─────────────────         ─────────
GPIO 7  (SWCLK)  ────────  SWCLK
GPIO 5  (SWDIO)  ───┬────  SWDIO
GPIO 6  (SWDIO)  ───┘
GPIO 3  (nRST)   ────────  nRST (可选)
GND               ────────  GND

GPIO 17 (TX)      ────────  RX  (串口桥接，可选)
GPIO 18 (RX)      ────────  TX
```

### 1.3 硬件要求

| 项目 | 规格 |
|------|------|
| MCU | ESP32-S3 |
| Flash | 16MB |
| PSRAM | 8MB Octal (OPI) |
| USB | ESP32-S3 内置 USB OTG |

---

## 2. 编译与烧录

### 2.1 环境准备

- **PlatformIO**：推荐通过 VS Code 插件安装
- **平台**：espressif32 6.11.0（ESP-IDF 5.4.1）

### 2.2 编译

```bash
cd esp32s3-idf
pio run
```

### 2.3 烧录

```bash
# 自动检测串口
pio run -t upload

# 指定串口
pio run -t upload --upload-port COM3
```

### 2.4 查看日志

```bash
pio device monitor -b 115200
```

### 2.5 擦除 Flash（全量重置）

```bash
pio run -t erase
```

### 2.6 Flash 分区布局

| 分区 | 偏移 | 大小 | 用途 |
|------|------|------|------|
| nvs | 0x9000 | 24KB | 非易失存储 |
| phy_init | 0xF000 | 4KB | 物理层初始化 |
| factory | 0x10000 | 7MB | 主固件 |
| mscfat | 0x710000 | 8MB | USB MSC FAT32 分区 |
| cache | 0xF10000 | 896KB | SPIFFS 缓存 |

---

## 3. USB 功能说明

ESP32-S3 通过一个 USB 口同时提供三种功能：

| 接口 | 类型 | 功能 |
|------|------|------|
| 接口 0 | Vendor (WinUSB) | CMSIS-DAP v2 调试 |
| 接口 1-2 | CDC-ACM | 虚拟串口（连接目标 UART） |
| 接口 3 | MSC | U 盘（拖拽烧录 + 配置文件） |

- **VID**: 0x0D28 (ARM)
- **PID**: 0x0204 (CMSIS-DAP)
- Windows 10+ 无需安装驱动（WinUSB 自动加载）

### 3.1 CMSIS-DAP v2（SWD 调试）

#### 使用方法

1. 将 ESP32-S3 通过 USB 连接电脑
2. 在 Keil/IAR/OpenOCD 中选择 CMSIS-DAP 调试器
3. 正常下载、调试

**Keil 设置**：
- Debug → Use: CMSIS-DAP Debugger
- Settings → SWD, 选择可用的 "DAPLink CMSIS-DAP"
- 推荐时钟频率：**1MHz**（兼顾速度与稳定性）

**OpenOCD 使用**：
```bash
openocd -f interface/cmsis-dap.cfg -f target/stm32g0x.cfg
```

#### 支持的 DAP 命令

| 命令 | 说明 |
|------|------|
| DAP_Info | 设备信息查询 |
| DAP_Connect / Disconnect | SWD 连接/断开 |
| DAP_TransferConfigure | 配置重试次数等 |
| DAP_Transfer | 单次 DP/AP 读写 |
| DAP_TransferBlock | 批量传输（用于内存读写） |
| DAP_WriteAbort | 清除 STICKYERR |
| SWJ_Pins | GPIO 引脚控制（含 nRST） |
| SWJ_Clock | 时钟频率设置 |
| SWJ_Sequence | JTAG-to-SWD 切换序列 |
| SWD_Configure | TurnAround / DataPhase |
| DAP_Delay | 延迟 |
| DAP_ResetTarget | 目标复位 |

> JTAG 不支持（仅 SWD）。

#### SWD 时钟频率

| 设定频率 | 实际表现 | 说明 |
|---------|---------|------|
| 10 MHz | ≈5-6 MHz | GPIO 位操作上限 |
| 1 MHz | 1 MHz | **推荐**，稳定可靠 |
| 500 KHz | 500 KHz | 正常 |
| 100 KHz | 100 KHz | 正常 |
| 50 KHz | 50 KHz | 长线缆可用 |

所有频率均使用 CPU 周期计数器精确延时，无 jitter。

#### FAULT 自动恢复

- SWD FAULT ACK 时自动发送 ABORT + 重试 1 次
- DAP_WriteAbort 无论 ACK 状态均返回成功（符合 ARM 规范）
- STICKYERR 在 Transfer/TransferBlock 中 FAULT 后自动清除

### 3.2 CDC 虚拟串口

电脑识别为虚拟 COM 口，可用任何串口工具（PuTTY、SecureCRT、SSCOM 等）打开。

- **默认波特率**：115200
- **数据格式**：8N1
- **支持动态变更波特率**：通过串口工具设置，自动同步到硬件 UART

**数据流**：
```
PC (COM 口) ←USB CDC→ ESP32 ←UART→ 目标 MCU
```

### 3.3 MSC 拖拽烧录

ESP32-S3 在电脑上显示为 U 盘，支持拖拽 HEX 文件自动烧录。

#### 使用步骤

1. 插入 ESP32-S3，电脑出现 U 盘
2. 将 `.hex` 文件复制/拖拽到 U 盘
3. 等待 2 秒（写入空闲检测）
4. 自动探测目标芯片 → 擦除 → 编程 → 校验
5. 查看结果：
   - 成功：`DETAILS.TXT` 记录烧录详情
   - 失败：`FAIL.TXT` 记录错误信息

#### U 盘文件结构

```
U 盘 (DAP_MSC)/
├── wifi.txt         ← WiFi 配置文件（见第4节）
├── algo/            ← FLM 闪存算法目录
│   └── *.flm       ← 自定义闪存算法文件
├── *.hex            ← 拖入此处自动烧录
├── DETAILS.TXT      ← 烧录成功详情
└── FAIL.TXT         ← 烧录失败信息
```

#### 编程流程

1. 检测文件写入完成（2 秒无新写入）
2. 解析 HEX 文件（Intel HEX 格式）
3. SWD 探测目标：DPIDR → 电源上电 → 读 IDCODE
4. 根据 DEV_ID 选择内置 Flash 算法或加载 FLM
5. 上传算法代码到目标 RAM
6. 逐段擦除 → 编程 → 校验
7. 写入状态文件

---

## 4. WiFi 无线调试

支持 WiFi 无线 SWD 调试，无需 USB 线连接电脑。详细文档见 [README_WIFI.md](../README_WIFI.md)。

### 4.1 快速开始（局域网直连）

1. 在 U 盘创建 `wifi.txt`：
   ```
   ssid=你的WiFi名
   pass=你的WiFi密码
   ```
2. 重启 ESP32-S3
3. 查看串口日志获取 IP 地址（如 `192.168.1.100`）
4. 在 Keil 中安装 [elaphureLink](https://github.com/windowsair/elern) 驱动
5. 配置 elaphureLink 连接 `192.168.1.100:3240`
6. 正常调试

### 4.2 跨网络调试（中继模式）

在 `wifi.txt` 中添加中继服务器地址：
```
ssid=你的WiFi名
pass=你的WiFi密码
relay=ws://你的服务器:7000
code=ABC123
```

PC 端运行 Bridge 工具连接同一中继服务器和配对码，即可跨网调试。

### 4.3 WiFi TCP 端口

| 端口 | 协议 | 用途 |
|------|------|------|
| 3240 | elaphureLink | DAP 命令传输 |
| 3241 | TCP | CDC 串口数据 |
| 3242 | TCP | RTT 数据 |

---

## 5. LED 指示灯

| 状态 | 绿灯 | 红灯 |
|------|------|------|
| 目标已连接（SWD OK） | 常亮 | 灭 |
| USB 已枚举，未连目标 | 慢闪（500ms） | 灭 |
| USB 未枚举 | 灭 | 慢闪（500ms） |

---

## 6. FLM 闪存算法

对于内置不支持的芯片，可使用 FLM 文件扩展支持。

### 6.1 什么是 FLM

FLM 是 Keil MDK 的闪存算法文件（ELF32 格式），包含：
- Flash 编程算法机器代码（运行在目标 RAM）
- Flash 设备描述（基地址、容量、扇区大小）
- 函数入口点（Init、EraseSector、ProgramPage 等）

### 6.2 使用方法

1. 从 Keil 安装目录找到目标芯片的 `.FLM` 文件
   - 通常位于 `Keil/ARM/Flash/` 目录
   - 例如：`STM32G0xx_64.FLM`
2. 将 FLM 文件复制到 U 盘的 `algo/` 目录
3. 拖拽 HEX 文件烧录时，如果内置算法不匹配，会自动尝试 `algo/` 目录下的 FLM

### 6.3 FLM 要求

- 格式：ELF32（ARM Cortex-M 目标代码）
- 必须包含标准 CMSIS Flash 接口函数
- 目标 MCU 必须有足够的 RAM 加载算法代码 + 数据缓冲

---

## 7. 支持的目标芯片

### 内置 Flash 算法

| 系列 | 型号 |
|------|------|
| **STM32G0** | G030, G031, G070, G071, G051, G061, G0B0, G0B1, G0C1 |
| **STM32G4** | G431, G441, G471, G473, G474, G483, G484, G491, G4A1 |
| **STM32F1** | F103 (Low/Medium/High density), F105, F107 |
| **STM32F4** | F401, F405, F407, F411, F415, F417 等 |

### 通过 FLM 扩展

理论上支持任何 ARM Cortex-M 系列 MCU，只需提供对应的 FLM 闪存算法文件。

---

## 8. PC 工具

### 8.1 PC Bridge（跨网桥接）

用于跨网络调试，在 PC 端作为 WebSocket → TCP 网关。

**位置**：`pc-bridge/`

#### 安装

```bash
cd pc-bridge
pip install -r requirements.txt
```

#### 使用

```bash
# 交互式（读取 config.ini）
python bridge.py

# 命令行指定参数
python bridge.py --relay ws://47.103.38.218:7000 --code CVSKQR
```

#### 配置文件 `config.ini`

```ini
[bridge]
relay = ws://47.103.38.218:7000    # 中继服务器地址
code = CVSKQR                      # 6 位配对码（与 wifi.txt 中一致）
port = 3240                         # 本地 DAP TCP 端口
serial_port = 3241                  # 本地串口 TCP 端口
rtt_port = 3242                     # 本地 RTT TCP 端口
```

#### 工作流程

```
Keil (elaLink) ──TCP 3240──► PC Bridge ──WebSocket──► 中继服务器 ──WebSocket──► ESP32
```

### 8.2 DAPLink Tool（综合工具）

一体化 GUI 工具，集成串口终端、WiFi 直连/中继连接、RTT 查看。

**位置**：`pc-tools/`

#### 安装

```bash
cd pc-tools
pip install -r requirements.txt
```

#### 使用

```bash
python daplink_tool.py
```

#### 功能

| 功能 | 说明 |
|------|------|
| 串口终端 | 选择 COM 口 + 波特率（9600-921600） |
| WiFi 直连 | 输入 ESP32 IP + 端口号 |
| 中继连接 | WebSocket 中继 + 配对码 |
| RTT 查看 | 实时 RTT 输出显示 |

#### 打包为 EXE

```bash
pip install pyinstaller
cd pc-tools
python build.bat
# 或手动:
pyinstaller --onefile --windowed daplink_tool.py
```

---

## 9. 中继服务器

用于跨网络调试的 WebSocket 转发服务器。

**位置**：`relay-server/`

### 部署

#### Docker（推荐）

```bash
cd relay-server
docker compose up -d --build
```

#### Node.js 直接运行

```bash
cd relay-server
npm install
node index.js       # 默认端口 7000
```

#### PM2 后台运行（生产环境）

```bash
npm install -g pm2
pm2 start index.js --name daplink-relay
pm2 save && pm2 startup
```

### 工作原理

```
ESP32      ──WebSocket──►  中继服务器  ◄──WebSocket──  PC Bridge
(role=device, code=ABC123)    (匹配配对码)    (role=client, code=ABC123)
```

通道多路复用（WebSocket 二进制帧首字节标识）：
- `0x00`：DAP 命令/响应
- `0x01`：串口数据
- `0x02`：RTT 数据

### 防火墙

确保服务器端口 **7000**（或自定义端口）对外开放。

---

## 10. 技术细节

### 10.1 USB 端点限制

ESP32-S3 DWC2 USB 控制器最多支持 **5 个 IN 端点**（含 EP0）。当前使用 4 个 IN 端点：

| 端点 | 接口 | 功能 |
|------|------|------|
| EP0 | 控制 | USB 枚举 |
| EP1 IN | Vendor | CMSIS-DAP 响应 |
| EP2 IN | CDC Comm | 串口通知 |
| EP3 IN | CDC Data | 串口数据 |
| EP4 IN | MSC | U 盘数据 |

### 10.2 SWD 实现

- **位操作**：直接寄存器访问（`GPIO.out_w1ts` / `GPIO.out_w1tc` / `GPIO.in`），亚微秒级
- **时钟延时**：CPU 周期计数器（`esp_cpu_get_cycle_count()`），无 jitter
- **分离 SWDIO**：输入（GPIO5）与输出（GPIO6）分开，避免方向切换延迟
- **互斥锁**：FreeRTOS mutex 保护多任务 SWD 总线访问

### 10.3 CMSIS-DAP 处理模型

所有 DAP 命令在 TinyUSB 任务回调中**同步处理**（`tud_vendor_rx_cb`）：

```
USB 包到达 → tud_vendor_rx_cb → swd::lock() → 处理命令 → swd::unlock() → 写响应
```

这保证了 `tud_vendor_write()` 在 TinyUSB 任务上下文中调用（线程安全要求）。

### 10.4 任务优先级

| 任务 | 核心 | 优先级 | 说明 |
|------|------|--------|------|
| TinyUSB | Core 0 | 5 | USB 协议栈 |
| app_main | Core 0 | 1 | 主循环（MSC扫描、LED） |
| usb_cdc_bridge | — | 24 | UART 数据搬运 |

### 10.5 WinUSB 免驱动

固件包含 MS OS 2.0 描述符，Windows 10+ 自动加载 WinUSB 驱动：
- BOS 描述符中的 MS OS 2.0 Platform Capability
- Compatible ID = "WINUSB"
- Device Interface GUID = `{CDB3B5AD-293B-4663-AA36-1AAE46463776}`

---

## 11. 已知限制

| 限制 | 说明 |
|------|------|
| 仅支持 SWD | 不支持 JTAG 协议 |
| 最大 SWD 频率 | ≈5-6 MHz（GPIO 位操作上限） |
| USB 包大小 | 64 字节（CMSIS-DAP v2 标准） |
| RTT | 暂时禁用（SWD 总线竞争问题待优化） |
| SWO | 不支持 |
| GPIO33-37 | 被 Octal PSRAM 占用，不可使用 |
