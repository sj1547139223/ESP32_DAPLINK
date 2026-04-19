/**
 * DAPLink WebSocket + TCP Relay Server  v1.0.0
 *
 * Pairs ESP32 DAPLink devices with PC bridge clients for cross-network debugging.
 *
 * WebSocket Relay (PORT, default 7000):
 *   1. Device connects and sends: {"type":"register","role":"device","code":"XXXXXX"}
 *   2. PC bridge connects and sends: {"type":"register","role":"client","code":"XXXXXX"}
 *   3. Server pairs them and sends: {"type":"paired"} to both
 *   4. Binary WebSocket frames are forwarded bidirectionally
 *
 * TCP Proxy (TCP_PORT, default 7001):
 *   1. Both device and client connect via raw TCP
 *   2. First message: [role(1), code_len(1), code(N)]  role: 0x01=device, 0x02=client
 *   3. Server responds: [0x00] (registered) or [0x01, peer_ip, peer_port] (paired)
 *   4. After pairing: raw bytes piped bidirectionally (no framing overhead)
 *
 * STUN-like UDP Endpoint (STUN_PORT, default 7002):
 *   Clients send any UDP packet to discover their public IP:port.
 *   Server responds: [port_hi, port_lo, ip_ascii...]
 *   Used for NAT hole punching to discover external UDP addresses.
 *
 * NAT Hole Punching (optional, via TCP control channel):
 *   After pairing, server sends peer's public IP:port to both sides.
 *   Clients can then attempt UDP hole punching for direct communication.
 *
 * Usage:
 *   PORT=7000 TCP_PORT=7001 STUN_PORT=7002 node index.js
 */

const { WebSocketServer } = require('ws');
const net = require('net');
const dgram = require('dgram');

const PORT = parseInt(process.env.PORT) || 7000;
const TCP_PORT = parseInt(process.env.TCP_PORT) || 7001;
const STUN_PORT = parseInt(process.env.STUN_PORT) || 7002;

// Room storage: code -> { device: ws, client: ws }
const rooms = new Map();

const wss = new WebSocketServer({ port: PORT });

console.log(`DAPLink Relay Server listening on port ${PORT}`);

wss.on('connection', (ws) => {
    let role = null;
    let code = null;

    ws.on('message', (data, isBinary) => {
        if (isBinary) {
            // Forward binary data to paired peer
            if (!code || !rooms.has(code)) return;
            const room = rooms.get(code);
            const peer = role === 'device' ? room.client : room.device;
            if (peer && peer.readyState === 1) {
                peer.send(data, { binary: true });
            }
            return;
        }

        // Text message: control plane
        try {
            const msg = JSON.parse(data.toString());

            if (msg.type === 'register' && msg.code && msg.role) {
                role = msg.role;
                code = msg.code.toUpperCase();

                if (!rooms.has(code)) {
                    rooms.set(code, { device: null, client: null });
                }

                const room = rooms.get(code);

                if (role === 'device') {
                    if (room.device) {
                        room.device.close();
                    }
                    room.device = ws;
                    console.log(`Device registered: ${code}`);
                } else if (role === 'client') {
                    if (room.client) {
                        room.client.close();
                    }
                    room.client = ws;
                    console.log(`Client registered: ${code}`);
                }

                // Check if both sides are connected
                if (room.device && room.client &&
                    room.device.readyState === 1 && room.client.readyState === 1) {
                    const pairMsg = JSON.stringify({ type: 'paired' });
                    room.device.send(pairMsg);
                    room.client.send(pairMsg);
                    console.log(`Paired: ${code}`);
                }
            }
        } catch (e) {
            // Ignore invalid JSON
        }
    });

    ws.on('close', () => {
        if (code && rooms.has(code)) {
            const room = rooms.get(code);
            if (role === 'device' && room.device === ws) {
                room.device = null;
                console.log(`Device disconnected: ${code}`);
                // Notify client
                if (room.client && room.client.readyState === 1) {
                    room.client.send(JSON.stringify({ type: 'device_disconnected' }));
                }
            } else if (role === 'client' && room.client === ws) {
                room.client = null;
                console.log(`Client disconnected: ${code}`);
            }
            // Clean up empty rooms
            if (!room.device && !room.client) {
                rooms.delete(code);
            }
        }
    });

    ws.on('error', (err) => {
        console.error('WebSocket error:', err.message);
    });
});

// Periodic cleanup of stale rooms
setInterval(() => {
    for (const [code, room] of rooms.entries()) {
        const devDead = !room.device || room.device.readyState !== 1;
        const cliDead = !room.client || room.client.readyState !== 1;
        if (devDead && cliDead) {
            rooms.delete(code);
        }
    }
}, 60000);

// ─── TCP Proxy Relay ─────────────────────────────────────────────────────────

const ROLE_DEVICE = 0x01;
const ROLE_CLIENT = 0x02;
const TCP_STATUS_REGISTERED = 0x00;
const TCP_STATUS_PAIRED = 0x01;
const TCP_STATUS_PEER_DISCONNECTED = 0x02;

// TCP room storage: code -> { device: socket, client: socket, deviceAddr, clientAddr, paired }
const tcpRooms = new Map();

const tcpServer = net.createServer({ noDelay: true }, (socket) => {
    let role = null;
    let code = null;
    let registered = false;
    let headerBuf = Buffer.alloc(0);

    socket.setNoDelay(true);
    socket.setKeepAlive(true, 10000);

    socket.on('data', (data) => {
        if (!registered) {
            // Accumulate header bytes
            headerBuf = Buffer.concat([headerBuf, data]);
            if (headerBuf.length < 2) return;

            const roleId = headerBuf[0];
            const codeLen = headerBuf[1];
            if (headerBuf.length < 2 + codeLen) return;

            code = headerBuf.slice(2, 2 + codeLen).toString('ascii').toUpperCase();
            role = roleId === ROLE_DEVICE ? 'device' : roleId === ROLE_CLIENT ? 'client' : null;

            if (!role || !code) {
                socket.destroy();
                return;
            }

            registered = true;
            const extra = headerBuf.slice(2 + codeLen); // any leftover data after header

            if (!tcpRooms.has(code)) {
                tcpRooms.set(code, { device: null, client: null, deviceAddr: null, clientAddr: null, paired: false });
            }

            const room = tcpRooms.get(code);

            // Close previous connection of same role
            if (role === 'device' && room.device) {
                room.device.destroy();
            } else if (role === 'client' && room.client) {
                room.client.destroy();
            }

            const addr = { ip: socket.remoteAddress, port: socket.remotePort };
            if (role === 'device') {
                room.device = socket;
                room.deviceAddr = addr;
            } else {
                room.client = socket;
                room.clientAddr = addr;
            }

            console.log(`TCP ${role} registered: ${code} from ${addr.ip}:${addr.port}`);

            // Check if both sides are connected
            if (room.device && room.client && !room.device.destroyed && !room.client.destroyed) {
                // If device was previously paired, send disconnect first (as length-prefixed)
                // so it resets from paired/length-prefixed mode to non-paired mode
                if (room.paired) {
                    // Reset paired state. Do NOT send length-prefixed disconnect here
                    // because the new device is in non-paired mode (expects single status bytes).
                    // Sending [0x00, 0x01, 0x02] would corrupt the protocol parsing.
                    room.paired = false;
                }

                // Send paired notification with peer address info (for hole punching)
                const devInfo = room.deviceAddr;
                const cliInfo = room.clientAddr;

                // To device: send client's public address
                const cliIpBuf = Buffer.from(cliInfo.ip.replace('::ffff:', ''));
                const devPairMsg = Buffer.alloc(4 + cliIpBuf.length);
                devPairMsg[0] = TCP_STATUS_PAIRED;
                devPairMsg.writeUInt16BE(cliInfo.port, 1);
                devPairMsg[3] = cliIpBuf.length;
                cliIpBuf.copy(devPairMsg, 4);
                room.device.write(devPairMsg);

                // To client: send device's public address
                const devIpBuf = Buffer.from(devInfo.ip.replace('::ffff:', ''));
                const cliPairMsg = Buffer.alloc(4 + devIpBuf.length);
                cliPairMsg[0] = TCP_STATUS_PAIRED;
                cliPairMsg.writeUInt16BE(devInfo.port, 1);
                cliPairMsg[3] = devIpBuf.length;
                devIpBuf.copy(cliPairMsg, 4);
                room.client.write(cliPairMsg);

                room.paired = true;
                console.log(`TCP Paired: ${code}`);
            } else {
                socket.write(Buffer.from([TCP_STATUS_REGISTERED]));
            }

            // Forward any leftover data after header
            if (extra.length > 0 && registered) {
                const room2 = tcpRooms.get(code);
                if (room2) {
                    const peer = role === 'device' ? room2.client : room2.device;
                    if (peer && !peer.destroyed) {
                        peer.write(extra);
                    }
                }
            }

            headerBuf = null; // free
            return;
        }
        // After registration, forward data to paired peer
        if (code && tcpRooms.has(code)) {
            const room = tcpRooms.get(code);
            const peer = role === 'device' ? room.client : room.device;
            if (peer && !peer.destroyed) {
                peer.write(data);
            }
        }
    });

    socket.on('close', () => {
        if (code && tcpRooms.has(code)) {
            const room = tcpRooms.get(code);
            if (role === 'device' && room.device === socket) {
                room.device = null;
                room.deviceAddr = null;
                console.log(`TCP device disconnected: ${code}`);
                if (room.client && !room.client.destroyed) {
                    if (room.paired) {
                        room.client.write(Buffer.from([0x00, 0x01, TCP_STATUS_PEER_DISCONNECTED]));
                    } else {
                        room.client.write(Buffer.from([TCP_STATUS_PEER_DISCONNECTED]));
                    }
                }
                room.paired = false;
            } else if (role === 'client' && room.client === socket) {
                room.client = null;
                room.clientAddr = null;
                console.log(`TCP client disconnected: ${code}`);
                // Notify device that client disconnected
                if (room.device && !room.device.destroyed) {
                    if (room.paired) {
                        room.device.write(Buffer.from([0x00, 0x01, TCP_STATUS_PEER_DISCONNECTED]));
                    } else {
                        room.device.write(Buffer.from([TCP_STATUS_PEER_DISCONNECTED]));
                    }
                }
                room.paired = false;
            }
            if (!room.device && !room.client) {
                tcpRooms.delete(code);
            }
        }
    });

    socket.on('error', (err) => {
        console.error('TCP error:', err.message);
    });
});

tcpServer.listen(TCP_PORT, () => {
    console.log(`TCP Proxy Relay listening on port ${TCP_PORT}`);
});

// Cleanup stale TCP rooms
setInterval(() => {
    for (const [code, room] of tcpRooms.entries()) {
        const devDead = !room.device || room.device.destroyed;
        const cliDead = !room.client || room.client.destroyed;
        if (devDead && cliDead) {
            tcpRooms.delete(code);
        }
    }
}, 60000);

// ─── STUN-like UDP Endpoint ──────────────────────────────────────────────────

const stunServer = dgram.createSocket('udp4');

stunServer.on('message', (msg, rinfo) => {
    // Reply with sender's public address: [port_hi, port_lo, ip_ascii...]
    const ip = rinfo.address.replace('::ffff:', '');
    const resp = Buffer.alloc(2 + ip.length);
    resp.writeUInt16BE(rinfo.port, 0);
    resp.write(ip, 2, 'ascii');
    stunServer.send(resp, rinfo.port, rinfo.address);
});

stunServer.on('error', (err) => {
    console.error('STUN error:', err.message);
});

stunServer.bind(STUN_PORT, () => {
    console.log(`STUN UDP endpoint on port ${STUN_PORT}`);
});
