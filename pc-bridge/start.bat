@echo off
chcp 65001 >nul 2>&1
title DAPLink 无线调试桥接工具

echo ══════════════════════════════════
echo   DAPLink 无线调试桥接工具
echo ══════════════════════════════════
echo.

REM 检查 Python
python --version >nul 2>&1
if %errorlevel% neq 0 (
    echo [错误] 未找到 Python，请从 https://www.python.org/downloads/ 安装
    echo        安装时请勾选 "Add Python to PATH"
    pause
    exit /b 1
)

REM 检查并安装依赖
python -c "import websockets" >nul 2>&1
if %errorlevel% neq 0 (
    echo [*] 正在安装依赖...
    pip install websockets -q
)

REM 启动桥接工具
python "%~dp0bridge.py" %*
pause
