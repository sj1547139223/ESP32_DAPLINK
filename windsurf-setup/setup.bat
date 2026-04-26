@echo off
chcp 65001 >nul
title Windsurf IDE 环境一键配置
echo.
echo  双击此文件即可开始配置 Windsurf 开发环境
echo.
powershell -ExecutionPolicy Bypass -File "%~dp0setup.ps1"
