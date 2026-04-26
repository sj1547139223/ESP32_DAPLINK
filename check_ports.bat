@echo off
echo 检查端口占用情况...
echo.
echo === 检查 3240 端口 (DAP) ===
netstat -ano | findstr :3240
echo.
echo === 检查 3241 端口 (串口) ===
netstat -ano | findstr :3241
echo.
echo === 检查 3242 端口 (RTT) ===
netstat -ano | findstr :3242
echo.
echo 如果看到 LISTENING 状态，说明端口已被占用
echo 如果看到 ESTABLISHED 状态，说明有连接正在使用
echo.
pause
