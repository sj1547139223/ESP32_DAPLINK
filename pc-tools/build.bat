@echo off
chcp 65001 >nul
echo ========================================
echo DAPLink Tool 打包脚本
echo ========================================
echo.
pip install pyinstaller pyserial websockets
echo.
echo 正在打包...
pyinstaller --onefile --windowed --name DAPLinkTool --add-data "elaphureLink_Windows_x64_release;elaphureLink_Windows_x64_release" daplink_tool.py
echo.
echo 完成！exe 在 dist\DAPLinkTool.exe
pause