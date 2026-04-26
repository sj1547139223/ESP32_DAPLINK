<#
.SYNOPSIS
    Windsurf IDE 一键环境配置脚本 (ESP32 DAPLink 项目)
.DESCRIPTION
    离线安装所有扩展、配置 settings.json、补丁 PlatformIO 跳过联网检查。
    无需代理即可完成全部配置。
.NOTES
    前置要求:
      1. 已安装 Windsurf IDE
      2. 已安装 PlatformIO Core (通过 pip install platformio 或官方安装器)
      3. 已安装 Node.js (用于执行 PIO 补丁脚本)
.USAGE
    以管理员或普通用户运行:
    powershell -ExecutionPolicy Bypass -File setup.ps1
#>

$ErrorActionPreference = "Continue"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  Windsurf IDE 环境一键配置工具" -ForegroundColor Cyan
Write-Host "  ESP32 DAPLink 项目开发环境" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""

# ---- 1. 检测 Windsurf ----
Write-Host "[1/5] 检测 Windsurf 安装..." -ForegroundColor Yellow
$windsurfCmd = $null

# 常见安装路径
$searchPaths = @(
    "D:\Programs\Windsurf\bin\windsurf.cmd",
    "C:\Program Files\Windsurf\bin\windsurf.cmd",
    "$env:LOCALAPPDATA\Programs\Windsurf\bin\windsurf.cmd"
)

foreach ($p in $searchPaths) {
    if (Test-Path $p) { $windsurfCmd = $p; break }
}

if (-not $windsurfCmd) {
    # 尝试从进程查找
    $proc = Get-Process Windsurf -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($proc -and $proc.Path) {
        $windsurfCmd = Join-Path (Split-Path $proc.Path) "bin\windsurf.cmd"
    }
}

if (-not $windsurfCmd) {
    # 尝试 PATH
    $found = Get-Command windsurf -ErrorAction SilentlyContinue
    if ($found) { $windsurfCmd = $found.Source }
}

if (-not $windsurfCmd -or -not (Test-Path $windsurfCmd)) {
    Write-Host "  错误: 未找到 Windsurf IDE，请先安装 Windsurf" -ForegroundColor Red
    Write-Host "  下载地址: https://windsurf.com" -ForegroundColor Gray
    Read-Host "按回车退出"
    exit 1
}
Write-Host "  找到 Windsurf: $windsurfCmd" -ForegroundColor Green

# ---- 2. 安装扩展 ----
Write-Host ""
Write-Host "[2/5] 安装离线扩展..." -ForegroundColor Yellow
$extDir = Join-Path $scriptDir "extensions"

if (-not (Test-Path $extDir)) {
    Write-Host "  错误: extensions 目录不存在" -ForegroundColor Red
    exit 1
}

$vsixFiles = Get-ChildItem $extDir -Filter "*.vsix" | Sort-Object Name
$total = $vsixFiles.Count
$current = 0
$failed = @()

foreach ($vsix in $vsixFiles) {
    $current++
    $name = $vsix.BaseName -replace '-\d+\.\d+.*$', ''
    Write-Host "  [$current/$total] $name ... " -NoNewline
    
    $result = & $windsurfCmd --install-extension $vsix.FullName --force 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host "OK" -ForegroundColor Green
    } else {
        Write-Host "失败" -ForegroundColor Red
        $failed += $vsix.Name
    }
}

if ($failed.Count -gt 0) {
    Write-Host "  以下扩展安装失败:" -ForegroundColor Red
    $failed | ForEach-Object { Write-Host "    - $_" -ForegroundColor Red }
}

# ---- 3. 配置 settings.json ----
Write-Host ""
Write-Host "[3/5] 配置 settings.json..." -ForegroundColor Yellow
$settingsDir = "$env:APPDATA\Windsurf\User"
$settingsFile = Join-Path $settingsDir "settings.json"
$templateFile = Join-Path $scriptDir "settings.json"

if (-not (Test-Path $settingsDir)) {
    New-Item -ItemType Directory -Path $settingsDir -Force | Out-Null
}

if (Test-Path $settingsFile) {
    # 合并现有配置
    try {
        $existing = Get-Content $settingsFile -Raw | ConvertFrom-Json
        $template = Get-Content $templateFile -Raw | ConvertFrom-Json
        
        # 只添加缺少的键
        $templateProps = $template.PSObject.Properties
        $modified = $false
        foreach ($prop in $templateProps) {
            if (-not $existing.PSObject.Properties[$prop.Name]) {
                $existing | Add-Member -NotePropertyName $prop.Name -NotePropertyValue $prop.Value
                $modified = $true
            }
        }
        
        if ($modified) {
            $existing | ConvertTo-Json -Depth 10 | Set-Content $settingsFile -Encoding UTF8
            Write-Host "  已合并配置到现有 settings.json" -ForegroundColor Green
        } else {
            Write-Host "  settings.json 已包含所有必要配置" -ForegroundColor Green
        }
    } catch {
        Write-Host "  合并失败，备份并覆盖..." -ForegroundColor Yellow
        Copy-Item $settingsFile "$settingsFile.bak" -Force
        Copy-Item $templateFile $settingsFile -Force
        Write-Host "  已覆盖 (旧文件备份为 settings.json.bak)" -ForegroundColor Green
    }
} else {
    Copy-Item $templateFile $settingsFile -Force
    Write-Host "  已创建 settings.json" -ForegroundColor Green
}

# ---- 4. 补丁 PlatformIO ----
Write-Host ""
Write-Host "[4/5] 补丁 PlatformIO IDE (跳过联网检查)..." -ForegroundColor Yellow
$patchScript = Join-Path $scriptDir "patch_pio.js"

# 检查 PIO Core 是否已安装
$pioExe = "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe"
if (Test-Path $pioExe) {
    $nodeCmd = Get-Command node -ErrorAction SilentlyContinue
    if ($nodeCmd) {
        $result = & node $patchScript 2>&1
        $result | ForEach-Object { Write-Host "  $_" }
    } else {
        Write-Host "  警告: 未找到 Node.js，跳过 PIO 补丁" -ForegroundColor Yellow
        Write-Host "  请手动运行: node patch_pio.js" -ForegroundColor Yellow
    }
} else {
    Write-Host "  PlatformIO Core 未安装，跳过补丁" -ForegroundColor Yellow
    Write-Host "  安装后请手动运行: node patch_pio.js" -ForegroundColor Yellow
}

# ---- 5. 完成 ----
Write-Host ""
Write-Host "[5/5] 清理和验证..." -ForegroundColor Yellow
$installed = & $windsurfCmd --list-extensions 2>&1
$count = ($installed | Where-Object { $_ -notmatch 'undefined_publisher' }).Count
Write-Host "  已安装 $count 个扩展" -ForegroundColor Green

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  配置完成!" -ForegroundColor Green
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "后续步骤:" -ForegroundColor Yellow
Write-Host "  1. 重启 Windsurf" 
Write-Host "  2. 打开项目目录: esp32s3-idf/"
Write-Host "  3. PlatformIO 应自动识别项目并加载"
Write-Host ""
Write-Host "如果 PlatformIO 仍卡住:" -ForegroundColor Yellow
Write-Host "  - 确认已安装 PIO Core: pip install platformio"
Write-Host "  - 重新运行补丁: node patch_pio.js"
Write-Host "  - 或配置代理: settings.json 中设置 http.proxy"
Write-Host ""

Read-Host "按回车退出"
