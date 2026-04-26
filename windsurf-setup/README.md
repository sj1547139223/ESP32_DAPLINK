# Windsurf IDE 环境一键配置工具

## 适用项目
ESP32 DAPLink 开发环境 — 包含 PlatformIO + ESP-IDF + Keil + Embedded IDE 全套工具链。

## 前置要求
1. **Windsurf IDE** — [下载](https://windsurf.com)
2. **PlatformIO Core** — 通过 `pip install platformio` 安装（或使用官方安装器）
3. **Node.js** — 用于 PIO 离线补丁（可选，仅补丁时需要）

## 一键安装

**双击 `setup.bat`** 即可，脚本会自动完成：

| 步骤 | 内容 |
|------|------|
| 1 | 检测 Windsurf 安装位置 |
| 2 | 离线安装 16 个预打包扩展 |
| 3 | 配置 settings.json |
| 4 | 补丁 PlatformIO（跳过联网检查） |
| 5 | 验证安装结果 |

## 包含的扩展

### 嵌入式开发
- **PlatformIO IDE** — ESP32/STM32 编译、烧录、调试
- **Arm Keil Studio Pack** — MDK v6 支持
- **Embedded IDE (EIDE)** — Keil/IAR/GCC 项目集成
- **ARM Assembly** — ARM 汇编语法高亮
- **Clangd** — C/C++ 智能提示（比 cpptools 更快）

### 调试工具
- **CDT GDB** — GDB 调试适配器
- **Memory Inspector** — 内存查看
- **Peripheral Inspector** — 外设寄存器查看

### Python 开发
- **Python** — Python 语言支持
- **Debugpy** — Python 调试器
- **Python Environments** — 虚拟环境管理

### 通用工具
- **Code Runner** — 一键运行代码
- **C/C++ Flylint** — 代码检查
- **YAML** — YAML 语法支持
- **中文语言包** — 界面汉化

## PlatformIO 联网问题说明

### 问题
PlatformIO 服务器 (`lb1.platformio.org`) 在中国大陆无法直接访问，
导致 PlatformIO IDE 扩展初始化时显示 **"Initializing PlatformIO Core..."** 永久卡住。

### 解决方案
`patch_pio.js` 脚本修改扩展代码，跳过联网检查，直接使用本地已安装的 PIO Core。

### 手动补丁
```powershell
node patch_pio.js
```

### 如果有代理
在 `settings.json` 中设置：
```json
{
    "http.proxy": "http://127.0.0.1:10808"
}
```

## 目录结构
```
windsurf-setup/
├── setup.bat          # 双击运行
├── setup.ps1          # PowerShell 安装脚本
├── patch_pio.js       # PlatformIO 离线补丁
├── settings.json      # Windsurf 配置模板
├── README.md          # 本文件
└── extensions/        # 预打包的 .vsix 扩展文件
    ├── arm.environment-manager-*.vsix
    ├── arm.keil-studio-pack-*.vsix
    ├── cl.eide-*.vsix
    ├── lordimmaculate.platformio-ide-*.vsix
    └── ... (共16个)
```

## 更新扩展包
如需更新扩展版本，在已配置好的电脑上运行：
```powershell
# 导出当前所有扩展
$extBase = "$env:USERPROFILE\.windsurf\extensions"
$outDir = ".\extensions"
Get-ChildItem $extBase -Directory | Where-Object {
    $_.Name -notmatch 'windsurf-login-helper|windsurfpyright'
} | ForEach-Object {
    $zip = "$outDir\$($_.Name).zip"
    $vsix = "$outDir\$($_.Name).vsix"
    Compress-Archive -Path "$($_.FullName)\*" -DestinationPath $zip -Force
    Rename-Item $zip $vsix -Force
    Write-Host "Packed: $($_.Name)"
}
```
