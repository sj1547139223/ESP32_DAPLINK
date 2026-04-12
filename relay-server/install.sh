#!/bin/bash
# DAPLink Relay Server - 一键部署脚本
# 支持: Ubuntu/Debian/CentOS 等 Linux 服务器
# 用法: bash install.sh

set -e

echo "========================================"
echo "  DAPLink 中继服务器 一键部署"
echo "========================================"
echo ""

# 检查是否已安装 Docker
if command -v docker &> /dev/null; then
    echo "[✓] Docker 已安装"
else
    echo "[*] 正在安装 Docker..."
    curl -fsSL https://get.docker.com | sh
    systemctl enable docker
    systemctl start docker
    echo "[✓] Docker 安装完成"
fi

# 检查是否已安装 docker-compose
if command -v docker-compose &> /dev/null || docker compose version &> /dev/null 2>&1; then
    echo "[✓] Docker Compose 已安装"
else
    echo "[*] 正在安装 Docker Compose..."
    apt-get install -y docker-compose-plugin 2>/dev/null || \
    yum install -y docker-compose-plugin 2>/dev/null || \
    pip install docker-compose 2>/dev/null
    echo "[✓] Docker Compose 安装完成"
fi

echo ""
echo "[*] 正在构建并启动中继服务器..."

# 使用 docker compose (v2) 或 docker-compose (v1)
if docker compose version &> /dev/null 2>&1; then
    docker compose up -d --build
else
    docker-compose up -d --build
fi

echo ""
echo "========================================"
echo "  部署完成！"
echo "========================================"
echo ""

# 获取公网 IP
PUBLIC_IP=$(curl -s ifconfig.me 2>/dev/null || curl -s ip.sb 2>/dev/null || echo "你的服务器IP")
echo "中继服务器地址: ws://${PUBLIC_IP}:7000"
echo ""
echo "ESP32 wifi.txt 配置:"
echo "  relay=ws://${PUBLIC_IP}:7000"
echo ""
echo "管理命令:"
echo "  查看状态: docker compose logs -f"
echo "  停止服务: docker compose down"
echo "  重启服务: docker compose restart"
echo ""
