# DAPLink Relay Server v1.0.0

WebSocket 中继服务器，用于跨网络无线 DAPLink 调试。

## 版本历史

| 版本 | 修改内容 |
|------|----------|
| **v1.0.0** | 初始版本。WebSocket中继(端口7000)、TCP代理(端口7001)、STUN UDP(端口7002)、设备配对码机制、NAT打洞支持、Docker一键部署 |

> 版本号定义在 `package.json` → `version` 及 `index.js` 文件头。

## 一键部署（推荐）

将整个 `relay-server/` 文件夹上传到服务器后执行：

```bash
cd relay-server
bash install.sh
```

脚本会自动安装 Docker 并启动服务，完成后打印中继地址。

## Docker 手动部署

```bash
docker compose up -d --build
```

管理命令：
```bash
docker compose logs -f       # 查看日志
docker compose down          # 停止
docker compose restart       # 重启
```

## 直接运行（需要 Node.js）

```bash
npm install
node index.js                # 默认端口 7000
PORT=8080 node index.js      # 自定义端口
```

后台运行推荐 PM2：
```bash
npm install -g pm2
pm2 start index.js --name daplink-relay
pm2 save && pm2 startup
```

## 防火墙

确保 **7000 端口** 已开放（云服务器还需在安全组中放行）。

## 工作原理

```
ESP32 DAPLink ──WebSocket──> Relay Server <──WebSocket── PC Bridge
     │                           │                           │
     │   {"register","device"}   │                           │
     ├──────────────────────────>│                           │
     │                           │   {"register","client"}   │
     │                           │<──────────────────────────┤
     │       {"paired"}          │        {"paired"}         │
     │<──────────────────────────┤──────────────────────────>│
     │                           │                           │
     │   [binary DAP cmd] ───────┼──────────────────────────>│
     │<──────────────────────────┼───── [binary DAP resp]    │
```

## 配对码

ESP32 启动后会生成 6 位配对码（如 `A3K7N2`），显示在串口日志中。
PC 端桥接工具输入相同配对码即可建立连接。
