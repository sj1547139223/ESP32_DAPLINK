#include "wifi_debug.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "cmsis_dap.h"
#include "board_config.h"
#include "flash_algo.h"
#include "hex_parser.h"
#include "target_probe.h"
#include "usb_cdc_bridge.h"
#include "usb_msc_volume.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_task_wdt.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_websocket_client.h"
#include "driver/uart.h"

namespace {

const char *kTag = "wifi_debug";

// ----- Configuration -----
constexpr int kElTcpPort = 3240;          // elaphureLink TCP server port
constexpr int kSerialTcpPort = 3241;      // Serial bridge TCP port
constexpr int kRttTcpPort = 3242;         // RTT TCP port
constexpr size_t kBufSize = 1500;         // elaphureLink recommends >= 1400
constexpr uint32_t kElIdentifier = 0x8a656c70;
constexpr uint32_t kElCmdHandshake = 0x00000000;
constexpr uint32_t kElDapVersion = 0x00000001;

// WiFi config (read from /usb/wifi.txt)
char g_ssid[33] = {};
char g_password[65] = {};
char g_relay_url[256] = {};  // e.g. ws://relay.example.com:7000 or tcp://host:7001
char g_device_code[16] = {}; // 6-char pairing code

// TCP relay mode (when relay URL starts with tcp://)
bool g_tcp_relay_mode = false;
char g_tcp_relay_host[128] = {};
int g_tcp_relay_port = 7001;

bool g_wifi_configured = false;
bool g_wifi_connected = false;
char g_ip_str[16] = {};
EventGroupHandle_t g_wifi_events = nullptr;
constexpr int kWifiConnectedBit = BIT0;

// TCP server state
int g_listen_fd = -1;
int g_client_fd = -1;
bool g_el_handshake_done = false;

// WebSocket relay state
esp_websocket_client_handle_t g_ws_client = nullptr;
bool g_ws_connected = false;
bool g_ws_paired = false;

// DAP command queue (replaces single-entry g_ws_cmd_buf to prevent overwrite)
struct DapCmdMsg {
    uint8_t data[kBufSize];
    uint16_t len;
    bool from_udp;
};
constexpr int kDapCmdQueueDepth = 8;
static QueueHandle_t g_dap_cmd_queue = nullptr;

// TCP relay state
int g_tcp_relay_fd = -1;
bool g_tcp_relay_connected = false;
bool g_tcp_relay_paired = false;
uint8_t g_tcp_rx_buf[4096];     // stream reassembly buffer
int g_tcp_rx_len = 0;

// Shared buffers (only accessed from wifi_debug task)
uint8_t g_recv_buf[kBufSize];
uint8_t g_resp_buf[kBufSize];

// Serial TCP bridge state
int g_serial_listen_fd = -1;
int g_serial_client_fd = -1;
uint8_t g_serial_buf[256];

// WebSocket relay channel prefixes (multiplex DAP + serial + RTT + flash)
constexpr uint8_t kWsChannelDap = 0x00;
constexpr uint8_t kWsChannelSerial = 0x01;
constexpr uint8_t kWsChannelRtt = 0x02;
constexpr uint8_t kWsChannelFlash = 0x03;
constexpr uint8_t kWsChannelPunch = 0x04;  // NAT hole punch coordination

// Punch sub-commands
constexpr uint8_t kPunchMyPort = 0x01;     // [0x04, 0x01, port_hi, port_lo] = my UDP port
constexpr uint8_t kPunchDirect = 0x02;     // [0x04, 0x02] = switching to direct UDP for DAP

// Remote flash sub-commands (PC → ESP32)
constexpr uint8_t kFlashCmdStart = 0x01;
constexpr uint8_t kFlashCmdData = 0x02;
constexpr uint8_t kFlashCmdFinish = 0x03;

// Remote flash response status (ESP32 → PC)
constexpr uint8_t kFlashStatusOk = 0x00;
constexpr uint8_t kFlashStatusError = 0x01;
constexpr uint8_t kFlashStatusProgress = 0x02;

// Serial data received from relay
uint8_t g_ws_serial_buf[256];
int g_ws_serial_len = 0;
bool g_ws_serial_ready = false;

// RTT TCP server state
int g_rtt_listen_fd = -1;
int g_rtt_client_fd = -1;

// RTT data from relay
uint8_t g_ws_rtt_buf[256];
int g_ws_rtt_len = 0;
bool g_ws_rtt_ready = false;

// Remote flash state
std::vector<hex_parser::Segment> g_flash_segments;
bool g_flash_in_progress = false;

// NAT hole punch state
enum PunchState {
    PUNCH_IDLE = 0,
    PUNCH_STUN_PROBING,   // Sent STUN probe, waiting for response
    PUNCH_WAIT_PEER,      // Got our external port, sent to peer, waiting for peer's port
    PUNCH_PUNCHING,       // Both ports known, sending punch packets
    PUNCH_SUCCESS,        // Hole punched
    PUNCH_FAILED          // Timed out
};
PunchState g_punch_state = PUNCH_IDLE;
int g_punch_udp_fd = -1;
bool g_punch_dap_active = false;   // true = use direct UDP for DAP
uint16_t g_punch_peer_udp_port = 0;
uint16_t g_punch_my_ext_port = 0;     // my external UDP port (from STUN)
struct sockaddr_in g_punch_peer_addr = {};   // peer's public address (from pairing)
struct sockaddr_in g_punch_direct_addr = {}; // peer's actual address (from received UDP)
struct sockaddr_in g_punch_stun_addr = {};   // STUN server address
uint32_t g_punch_last_send_ms = 0;
uint32_t g_punch_start_ms = 0;
int g_punch_stun_retries = 0;
constexpr uint32_t kPunchIntervalMs = 100;   // Send punch every 100ms
constexpr uint32_t kPunchTimeoutMs = 15000;  // Give up after 15s
constexpr int kStunMaxRetries = 10;          // Max STUN probe retries
constexpr char kPunchMagic[] = "DAPLINK_PUNCH";
constexpr char kPunchAck[] = "DAPLINK_PUNCH_ACK";
constexpr int kStunPort = 7002;              // STUN UDP port on relay server

// Flash command queue (from WS event handler to main loop)
struct FlashMsg {
    uint8_t cmd;
    uint32_t address;           // for kFlashCmdData
    std::vector<uint8_t> data;  // for kFlashCmdData
};
QueueHandle_t g_flash_queue = nullptr;  // Queue of FlashMsg*

// ----- Relay Abstraction (WS or TCP) -----

void tcp_relay_send_raw(const uint8_t *data, size_t len);

// Forward declarations for NAT punch functions
void punch_start(const char *peer_ip, uint16_t peer_port);
void punch_handle_relay_msg(const uint8_t *payload, int len);
void punch_poll();
void punch_send_dap_response(const uint8_t *data, size_t len);

bool relay_is_paired() {
    if (g_tcp_relay_mode) return g_tcp_relay_paired;
    return g_ws_client && g_ws_paired;
}

// Send a channel-prefixed relay message (data[0] = channel)
void relay_send(const uint8_t *data, size_t len) {
    if (g_tcp_relay_mode) {
        tcp_relay_send_raw(data, len);
    } else if (g_ws_client) {
        esp_websocket_client_send_bin(g_ws_client, (const char *)data, len, portMAX_DELAY);
    }
}

void ws_send_flash_status(uint8_t status, const char *msg) {
    size_t msg_len = msg ? strlen(msg) : 0;
    size_t total = 2 + msg_len;  // [channel, status, msg...]
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return;
    buf[0] = kWsChannelFlash;
    buf[1] = status;
    if (msg_len > 0) memcpy(buf + 2, msg, msg_len);
    relay_send(buf, total);
    free(buf);
}

// ----- WiFi Configuration File Parser -----

bool g_config_read_ok = false;

void read_wifi_config_cb(void *ctx)
{
    (void)ctx;
    FILE *f = fopen("/usb/wifi.txt", "r");
    if (!f) {
        ESP_LOGI(kTag, "No /usb/wifi.txt found, WiFi debug disabled");
        g_config_read_ok = false;
        return;
    }

    char line[300];
    while (fgets(line, sizeof(line), f)) {
        // Remove trailing newline/CR
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0 || line[0] == '#') continue;

        if (strncmp(line, "ssid=", 5) == 0) {
            strncpy(g_ssid, line + 5, sizeof(g_ssid) - 1);
        } else if (strncmp(line, "pass=", 5) == 0) {
            strncpy(g_password, line + 5, sizeof(g_password) - 1);
        } else if (strncmp(line, "relay=", 6) == 0) {
            strncpy(g_relay_url, line + 6, sizeof(g_relay_url) - 1);
        }
    }
    fclose(f);

    if (g_ssid[0] == '\0') {
        ESP_LOGE(kTag, "wifi.txt: 'ssid=' not found");
        g_config_read_ok = false;
        return;
    }

    ESP_LOGI(kTag, "WiFi config: SSID=%s relay=%s", g_ssid,
             g_relay_url[0] ? g_relay_url : "(none)");

    // Detect TCP relay mode from URL prefix
    if (strncmp(g_relay_url, "tcp://", 6) == 0) {
        g_tcp_relay_mode = true;
        // Parse host:port from tcp://host:port
        const char *hp = g_relay_url + 6;
        const char *colon = strrchr(hp, ':');
        if (colon) {
            size_t host_len = colon - hp;
            if (host_len >= sizeof(g_tcp_relay_host)) host_len = sizeof(g_tcp_relay_host) - 1;
            memcpy(g_tcp_relay_host, hp, host_len);
            g_tcp_relay_host[host_len] = '\0';
            g_tcp_relay_port = atoi(colon + 1);
        } else {
            strncpy(g_tcp_relay_host, hp, sizeof(g_tcp_relay_host) - 1);
            g_tcp_relay_port = 7001;
        }
        ESP_LOGI(kTag, "TCP relay mode: host=%s port=%d", g_tcp_relay_host, g_tcp_relay_port);
    }

    g_config_read_ok = true;
}

// ----- WiFi Event Handler -----

void wifi_event_handler(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_connected = false;
        xEventGroupClearBits(g_wifi_events, kWifiConnectedBit);
        ESP_LOGW(kTag, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto *event = (ip_event_got_ip_t *)event_data;
        esp_ip4addr_ntoa(&event->ip_info.ip, g_ip_str, sizeof(g_ip_str));
        ESP_LOGW(kTag, "WiFi connected! IP: %s, elaphureLink port: %d", g_ip_str, kElTcpPort);
        g_wifi_connected = true;
        xEventGroupSetBits(g_wifi_events, kWifiConnectedBit);
        // Print IP+code to USB CDC for user who opened serial before WiFi connected
        usb_cdc_bridge::print_wifi_info();
    }
}

// ----- Generate Device Code -----

void generate_device_code()
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    // Deterministic seed from full MAC — same board always gets same code
    uint32_t seed = mac[0] | (mac[1] << 8) | (mac[2] << 16) | (mac[3] << 24);
    seed ^= mac[4] | (mac[5] << 8);
    const char charset[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // no 0OI1
    for (int i = 0; i < 6; i++) {
        g_device_code[i] = charset[seed % 32];
        seed = seed * 1103515245 + 12345;
    }
    g_device_code[6] = '\0';
    ESP_LOGW(kTag, "Device pairing code: %s (fixed per device)", g_device_code);
}

// ----- WiFi Init -----

esp_err_t init_wifi()
{
    // Initialize NVS (required by WiFi driver)
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(nvs_ret, kTag, "NVS init");

    g_wifi_events = xEventGroupCreate();

    ESP_RETURN_ON_ERROR(esp_netif_init(), kTag, "netif init");

    // Create default event loop (may already exist from other components)
    esp_err_t loop_ret = esp_event_loop_create_default();
    if (loop_ret != ESP_OK && loop_ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(loop_ret, kTag, "event loop");
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), kTag, "wifi init");

    esp_event_handler_instance_t inst_any, inst_got_ip;
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            &wifi_event_handler, nullptr, &inst_any),
        kTag, "register wifi event");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            &wifi_event_handler, nullptr, &inst_got_ip),
        kTag, "register ip event");

    wifi_config_t wifi_cfg = {};
    strncpy((char *)wifi_cfg.sta.ssid, g_ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, g_password, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = g_password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), kTag, "set STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), kTag, "set config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "wifi start");

    // --- WiFi scan for diagnostics ---
    esp_wifi_disconnect();  // stop auto-connect so scan can run
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGW(kTag, "Starting WiFi scan for diagnostics...");
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = true;
    esp_wifi_scan_start(&scan_cfg, true);  // blocking scan
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20;
    wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(ap_count * sizeof(wifi_ap_record_t));
    if (ap_list) {
        esp_wifi_scan_get_ap_records(&ap_count, ap_list);
        ESP_LOGW(kTag, "=== WiFi Scan Results (%d APs) ===", ap_count);
        for (int i = 0; i < ap_count; i++) {
            ESP_LOGW(kTag, "  [%d] SSID=%-32s RSSI=%d CH=%d Auth=%d",
                     i, ap_list[i].ssid, ap_list[i].rssi,
                     ap_list[i].primary, ap_list[i].authmode);
        }
        ESP_LOGW(kTag, "=== End of Scan ===");
        free(ap_list);
    }
    esp_wifi_connect();  // resume connection after scan
    // --- End scan ---

    ESP_LOGI(kTag, "WiFi STA starting, SSID=%s", g_ssid);
    return ESP_OK;
}

// ----- elaphureLink TCP Server -----

bool el_start_server()
{
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(kElTcpPort);

    g_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen_fd < 0) {
        ESP_LOGE(kTag, "socket() failed: %d", errno);
        return false;
    }

    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Set listen socket to non-blocking
    int flags = fcntl(g_listen_fd, F_GETFL, 0);
    fcntl(g_listen_fd, F_SETFL, flags | O_NONBLOCK);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(kTag, "bind() failed: %d", errno);
        close(g_listen_fd);
        g_listen_fd = -1;
        return false;
    }

    if (listen(g_listen_fd, 1) < 0) {
        ESP_LOGE(kTag, "listen() failed: %d", errno);
        close(g_listen_fd);
        g_listen_fd = -1;
        return false;
    }

    ESP_LOGI(kTag, "elaphureLink TCP server listening on port %d", kElTcpPort);
    return true;
}

void el_handle_handshake(const uint8_t *data, size_t len)
{
    if (len != 12) {
        ESP_LOGW(kTag, "EL handshake: bad length %d", (int)len);
        return;
    }

    uint32_t id, cmd, ver;
    memcpy(&id, data, 4);
    memcpy(&cmd, data + 4, 4);
    memcpy(&ver, data + 8, 4);
    id = ntohl(id);
    cmd = ntohl(cmd);
    ver = ntohl(ver);

    if (id != kElIdentifier || cmd != kElCmdHandshake) {
        ESP_LOGW(kTag, "EL handshake: bad id=0x%08lx cmd=0x%08lx",
                 (unsigned long)id, (unsigned long)cmd);
        return;
    }

    ESP_LOGI(kTag, "EL handshake OK, proxy version=%lu", (unsigned long)ver);

    // Send response
    uint8_t resp[12];
    uint32_t resp_id = htonl(kElIdentifier);
    uint32_t resp_cmd = htonl(kElCmdHandshake);
    uint32_t resp_ver = htonl(kElDapVersion);
    memcpy(resp, &resp_id, 4);
    memcpy(resp + 4, &resp_cmd, 4);
    memcpy(resp + 8, &resp_ver, 4);

    send(g_client_fd, resp, 12, 0);
    g_el_handshake_done = true;
}

// ----- DAP_ExecuteCommands (0x7F) support -----
// Computes the request size of a single CMSIS-DAP command.
// Returns 0 if command is unknown or buffer too short.
size_t dap_request_size(const uint8_t *cmd, size_t max_len)
{
    if (max_len < 1) return 0;

    switch (cmd[0]) {
    case 0x00: return 2;                         // DAP_Info
    case 0x01: return 3;                         // DAP_HostStatus
    case 0x02: return 2;                         // DAP_Connect
    case 0x03: return 1;                         // DAP_Disconnect
    case 0x04: return 6;                         // DAP_TransferConfigure
    case 0x07: return 1;                         // DAP_TransferAbort
    case 0x08: return 6;                         // DAP_WriteAbort
    case 0x09: return 3;                         // DAP_Delay
    case 0x0A: return 1;                         // DAP_ResetTarget
    case 0x10: return 7;                         // DAP_SWJ_Pins
    case 0x11: return 5;                         // DAP_SWJ_Clock
    case 0x13: return 2;                         // DAP_SWD_Configure

    case 0x12: {                                 // DAP_SWJ_Sequence
        if (max_len < 2) return 0;
        uint8_t bits = cmd[1];
        size_t byte_count = (bits == 0) ? 32 : ((bits + 7) / 8);
        return 2 + byte_count;
    }

    case 0x05: {                                 // DAP_Transfer
        if (max_len < 3) return 0;
        size_t pos = 3;  // cmd + dap_index + count
        uint8_t count = cmd[2];
        for (uint8_t i = 0; i < count; i++) {
            if (pos >= max_len) return 0;
            uint8_t req = cmd[pos++];
            bool is_read = (req & 0x02) != 0;
            bool is_match_mask = (req & 0x20) != 0;
            if (!is_read || is_match_mask) {
                pos += 4;  // write data or match mask value
            }
            // MatchValue (bit4) and Timestamp (bit7) don't add data bytes
        }
        return pos;
    }

    case 0x06: {                                 // DAP_TransferBlock
        if (max_len < 5) return 0;
        uint16_t count = cmd[2] | (cmd[3] << 8);
        uint8_t req = cmd[4];
        bool is_read = (req & 0x02) != 0;
        if (is_read) {
            return 5;
        } else {
            return 5 + (size_t)count * 4;
        }
    }

    default:
        return 0;  // Unknown command
    }
}

void el_process_execute_commands(const uint8_t *data, size_t len)
{
    if (len < 2) return;

    uint8_t num_cmds = data[1];
    size_t in_pos = 2;
    size_t out_pos = 2;  // Reserve 2 bytes for [0x7F, num]

    g_resp_buf[0] = 0x7F;
    g_resp_buf[1] = num_cmds;

    uint8_t sub_resp[256];  // Temp buffer for individual response

    for (uint8_t i = 0; i < num_cmds; i++) {
        if (in_pos >= len) break;

        size_t cmd_size = dap_request_size(data + in_pos, len - in_pos);
        if (cmd_size == 0 || in_pos + cmd_size > len) {
            ESP_LOGD(kTag, "ExecCmd: unknown cmd=0x%02x at pos=%d", data[in_pos], (int)in_pos);
            break;
        }

        size_t resp_len = cmsis_dap::process_command(
            data + in_pos, cmd_size, sub_resp, sizeof(sub_resp));

        if (resp_len > 0 && out_pos + resp_len < sizeof(g_resp_buf)) {
            memcpy(g_resp_buf + out_pos, sub_resp, resp_len);
            out_pos += resp_len;
        }

        in_pos += cmd_size;
    }

    send(g_client_fd, g_resp_buf, out_pos, 0);
}

void el_process_dap_command(const uint8_t *data, size_t len)
{
    if (len >= 2 && data[0] == 0x7F) {
        // DAP_ExecuteCommands - batch mode
        el_process_execute_commands(data, len);
    } else {
        size_t resp_len = cmsis_dap::process_command(data, len, g_resp_buf, sizeof(g_resp_buf));
        if (resp_len > 0) {
            send(g_client_fd, g_resp_buf, resp_len, 0);
        }
    }
}

void el_poll_tcp()
{
    if (g_listen_fd < 0) return;

    // Use select() to wait for activity (avoids busy-loop and watchdog triggers)
    fd_set read_fds;
    FD_ZERO(&read_fds);
    int max_fd = g_listen_fd;
    FD_SET(g_listen_fd, &read_fds);

    if (g_client_fd >= 0) {
        FD_SET(g_client_fd, &read_fds);
        if (g_client_fd > max_fd) max_fd = g_client_fd;
    }

    // Use short timeout when WS relay command is pending to minimize latency.
    // Otherwise 5ms timeout lets Core 1 yield for lwIP + IDLE WDT.
    bool dap_pending = g_dap_cmd_queue && uxQueueMessagesWaiting(g_dap_cmd_queue) > 0;
    int us = (dap_pending || g_ws_serial_ready || g_ws_rtt_ready) ? 0 : 5000;
    struct timeval tv = { .tv_sec = 0, .tv_usec = us };
    int sel = select(max_fd + 1, &read_fds, nullptr, nullptr, &tv);
    if (sel <= 0) return;

    // Accept new connections
    if (g_client_fd < 0 && FD_ISSET(g_listen_fd, &read_fds)) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        g_client_fd = accept(g_listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (g_client_fd >= 0) {
            char ip[16];
            inet_ntoa_r(client_addr.sin_addr, ip, sizeof(ip));
            ESP_LOGW(kTag, "EL client connected from %s", ip);
            g_el_handshake_done = false;

            // Set client socket to non-blocking
            int flags = fcntl(g_client_fd, F_GETFL, 0);
            fcntl(g_client_fd, F_SETFL, flags | O_NONBLOCK);

            // TCP_NODELAY for low latency
            int opt = 1;
            setsockopt(g_client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        }
    }

    if (g_client_fd < 0) return;

    // Read data from client
    if (!FD_ISSET(g_client_fd, &read_fds)) return;

    int n = recv(g_client_fd, g_recv_buf, sizeof(g_recv_buf), 0);
    if (n > 0) {
        if (!g_el_handshake_done) {
            el_handle_handshake(g_recv_buf, n);
        } else {
            el_process_dap_command(g_recv_buf, n);
        }
    } else if (n == 0) {
        // Client disconnected
        ESP_LOGW(kTag, "EL client disconnected");
        close(g_client_fd);
        g_client_fd = -1;
        g_el_handshake_done = false;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGW(kTag, "EL recv error: %d", errno);
        close(g_client_fd);
        g_client_fd = -1;
        g_el_handshake_done = false;
    }
}

// ----- WebSocket Relay Client -----

void ws_event_handler(void *handler_args, esp_event_base_t base,
                      int32_t event_id, void *event_data)
{
    auto *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(kTag, "WS relay connected");
        g_ws_connected = true;
        g_ws_paired = false;
        // Send registration message
        {
            char reg_msg[128];
            snprintf(reg_msg, sizeof(reg_msg),
                     "{\"type\":\"register\",\"role\":\"device\",\"code\":\"%s\"}",
                     g_device_code);
            esp_websocket_client_send_text(g_ws_client, reg_msg, strlen(reg_msg),
                                           portMAX_DELAY);
            ESP_LOGI(kTag, "WS registered with code: %s", g_device_code);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(kTag, "WS relay disconnected");
        g_ws_connected = false;
        g_ws_paired = false;
        // Resume USB CDC UART reading
        if (g_serial_client_fd < 0) {
            usb_cdc_bridge::set_uart_paused(false);
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x01) {
            // Text message (control plane)
            if (data->data_len > 0 && strstr((const char *)data->data_ptr, "\"paired\"")) {
                ESP_LOGW(kTag, "WS relay: paired with PC client!");
                g_ws_paired = true;
                // Pause USB CDC UART reading so relay serial works
                usb_cdc_bridge::set_uart_paused(true);
            }
        } else if (data->op_code == 0x02 && g_ws_paired) {
            // Binary message (from PC via relay) — channel-prefixed
            if (data->data_len > 1) {
                uint8_t channel = ((const uint8_t *)data->data_ptr)[0];
                const uint8_t *payload = ((const uint8_t *)data->data_ptr) + 1;
                int payload_len = data->data_len - 1;
                if (channel == kWsChannelDap && payload_len <= (int)kBufSize && g_dap_cmd_queue) {
                    DapCmdMsg *msg = new(std::nothrow) DapCmdMsg();
                    if (msg) {
                        memcpy(msg->data, payload, payload_len);
                        msg->len = payload_len;
                        msg->from_udp = false;
                        if (xQueueSend(g_dap_cmd_queue, &msg, 0) != pdTRUE) {
                            delete msg;
                        }
                    }
                } else if (channel == kWsChannelSerial && payload_len <= (int)sizeof(g_ws_serial_buf)) {
                    memcpy(g_ws_serial_buf, payload, payload_len);
                    g_ws_serial_len = payload_len;
                    g_ws_serial_ready = true;
                } else if (channel == kWsChannelRtt && payload_len <= (int)sizeof(g_ws_rtt_buf)) {
                    memcpy(g_ws_rtt_buf, payload, payload_len);
                    g_ws_rtt_len = payload_len;
                    g_ws_rtt_ready = true;
                } else if (channel == kWsChannelFlash && payload_len >= 1 && g_flash_queue) {
                    uint8_t cmd = payload[0];
                    FlashMsg *fm = new FlashMsg();
                    if (fm) {
                        fm->cmd = cmd;
                        if (cmd == kFlashCmdData && payload_len >= 5) {
                            // [cmd, addr_le(4), data...]
                            fm->address = payload[1] | (payload[2] << 8) |
                                          (payload[3] << 16) | (payload[4] << 24);
                            if (payload_len > 5) {
                                fm->data.assign(payload + 5, payload + payload_len);
                            }
                        }
                        if (xQueueSend(g_flash_queue, &fm, 0) != pdTRUE) {
                            delete fm;
                        }
                    }
                } else if (channel == kWsChannelPunch && payload_len >= 1) {
                    punch_handle_relay_msg(payload, payload_len);
                }
            } else if (data->data_len > 0) {
                // Legacy: no prefix, treat as DAP for backward compatibility
                if (data->data_len <= (int)kBufSize && g_dap_cmd_queue) {
                    DapCmdMsg *msg = new(std::nothrow) DapCmdMsg();
                    if (msg) {
                        memcpy(msg->data, data->data_ptr, data->data_len);
                        msg->len = data->data_len;
                        msg->from_udp = false;
                        if (xQueueSend(g_dap_cmd_queue, &msg, 0) != pdTRUE) {
                            delete msg;
                        }
                    }
                }
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(kTag, "WS error");
        break;

    default:
        break;
    }
}

void ws_start_relay()
{
    if (g_relay_url[0] == '\0') {
        ESP_LOGI(kTag, "No relay URL configured, relay mode disabled");
        return;
    }

    // Init DAP command queue
    if (!g_dap_cmd_queue) {
        g_dap_cmd_queue = xQueueCreate(kDapCmdQueueDepth, sizeof(DapCmdMsg *));
    }

    // Init flash command queue
    if (!g_flash_queue) {
        g_flash_queue = xQueueCreate(16, sizeof(FlashMsg *));
    }

    // Append device code as query parameter
    char url_with_params[512];
    snprintf(url_with_params, sizeof(url_with_params), "%s", g_relay_url);

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = url_with_params;
    ws_cfg.buffer_size = kBufSize;
    ws_cfg.reconnect_timeout_ms = 5000;
    ws_cfg.network_timeout_ms = 10000;

    g_ws_client = esp_websocket_client_init(&ws_cfg);
    if (!g_ws_client) {
        ESP_LOGE(kTag, "WS client init failed");
        return;
    }

    esp_websocket_register_events(g_ws_client, WEBSOCKET_EVENT_ANY,
                                  ws_event_handler, nullptr);
    esp_websocket_client_start(g_ws_client);
    ESP_LOGI(kTag, "WS relay client started: %s", g_relay_url);
}

void ws_poll_relay()
{
    if (!g_dap_cmd_queue) return;

    DapCmdMsg *msg = nullptr;
    if (xQueueReceive(g_dap_cmd_queue, &msg, 0) != pdTRUE || !msg) return;

    // Need either relay paired OR punch active
    if (!relay_is_paired() && !g_punch_dap_active) {
        delete msg;
        return;
    }

    bool via_udp = msg->from_udp;
    uint8_t *cmd_buf = msg->data;
    int cmd_len = msg->len;

    if (cmd_len >= 2 && cmd_buf[0] == 0x7F) {
        // DAP_ExecuteCommands batch — process sub-commands individually
        uint8_t num_cmds = cmd_buf[1];
        size_t in_pos = 2;
        size_t out_pos = via_udp ? 2 : 3;  // UDP: [0x7F, num], relay: [channel, 0x7F, num]

        if (!via_udp) g_resp_buf[0] = kWsChannelDap;
        g_resp_buf[via_udp ? 0 : 1] = 0x7F;
        g_resp_buf[via_udp ? 1 : 2] = num_cmds;

        uint8_t sub_resp[256];

        for (uint8_t i = 0; i < num_cmds; i++) {
            if (in_pos >= (size_t)cmd_len) break;

            size_t cmd_size = dap_request_size(cmd_buf + in_pos,
                                                cmd_len - in_pos);
            if (cmd_size == 0 || in_pos + cmd_size > (size_t)cmd_len) break;

            size_t resp_len = cmsis_dap::process_command(
                cmd_buf + in_pos, cmd_size, sub_resp, sizeof(sub_resp));

            if (resp_len > 0 && out_pos + resp_len < sizeof(g_resp_buf)) {
                memcpy(g_resp_buf + out_pos, sub_resp, resp_len);
                out_pos += resp_len;
            }

            in_pos += cmd_size;
        }

        if (via_udp) {
            // Send via direct UDP (with channel prefix for consistency)
            punch_send_dap_response(g_resp_buf, out_pos);
        } else {
            relay_send(g_resp_buf, out_pos);
        }
    } else {
        // Single CMSIS-DAP command
        if (via_udp) {
            size_t resp_len = cmsis_dap::process_command(
                cmd_buf, cmd_len, g_resp_buf, sizeof(g_resp_buf));
            if (resp_len > 0) {
                punch_send_dap_response(g_resp_buf, resp_len);
            }
        } else {
            size_t resp_len = cmsis_dap::process_command(
                cmd_buf, cmd_len, g_resp_buf + 1, sizeof(g_resp_buf) - 1);
            if (resp_len > 0) {
                g_resp_buf[0] = kWsChannelDap;
                relay_send(g_resp_buf, resp_len + 1);
            }
        }
    }

    delete msg;
}

void ws_poll_flash()
{
    if (!g_flash_queue || !relay_is_paired()) return;

    FlashMsg *fm = nullptr;
    if (xQueueReceive(g_flash_queue, &fm, 0) != pdTRUE || !fm) return;

    switch (fm->cmd) {
    case kFlashCmdStart: {
        ESP_LOGI(kTag, "Remote flash: START");
        g_flash_segments.clear();
        g_flash_in_progress = true;
        ws_send_flash_status(kFlashStatusOk, "ready");
        break;
    }
    case kFlashCmdData: {
        if (!g_flash_in_progress) {
            ws_send_flash_status(kFlashStatusError, "not started");
            break;
        }
        // Append or merge into segments
        bool merged = false;
        if (!g_flash_segments.empty()) {
            auto &last = g_flash_segments.back();
            if (fm->address == last.address + last.data.size()) {
                last.data.insert(last.data.end(), fm->data.begin(), fm->data.end());
                merged = true;
            }
        }
        if (!merged) {
            hex_parser::Segment seg;
            seg.address = fm->address;
            seg.data = std::move(fm->data);
            g_flash_segments.push_back(std::move(seg));
        }
        ESP_LOGD(kTag, "Remote flash: DATA addr=0x%08lx +%u bytes",
                 (unsigned long)fm->address, (unsigned)fm->data.size());
        ws_send_flash_status(kFlashStatusOk, "ok");
        break;
    }
    case kFlashCmdFinish: {
        if (!g_flash_in_progress || g_flash_segments.empty()) {
            ws_send_flash_status(kFlashStatusError, "no data");
            g_flash_in_progress = false;
            break;
        }
        ESP_LOGI(kTag, "Remote flash: PROGRAMMING %u segments", (unsigned)g_flash_segments.size());
        ws_send_flash_status(kFlashStatusProgress, "probing");

        target_probe::TargetInfo target;
        if (target_probe::probe(target) != ESP_OK) {
            ws_send_flash_status(kFlashStatusError, "probe failed");
            g_flash_in_progress = false;
            g_flash_segments.clear();
            break;
        }
        ws_send_flash_status(kFlashStatusProgress, target.name.c_str());

        hex_parser::ParsedHexImage image;
        image.segments = g_flash_segments;
        for (const auto &seg : g_flash_segments) {
            if (seg.address < image.lowest_address)
                image.lowest_address = seg.address;
        }
        const auto selection = flash_algo::select_algorithm(image, target);
        ESP_LOGI(kTag, "Remote flash: algo=%s target=%s",
                 selection.algorithm_name, target.name.c_str());
        ws_send_flash_status(kFlashStatusProgress, selection.algorithm_name);

        esp_err_t err = flash_algo::program_target(selection, target, g_flash_segments);

        if (err != ESP_OK) {
            // Try FLM fallback: switch to APP mount to read algo files from USB volume
            ESP_LOGW(kTag, "Built-in algorithm failed (%s), trying FLM fallback",
                     esp_err_to_name(err));
            ws_send_flash_status(kFlashStatusProgress, "trying FLM");

            struct FlmCtx {
                esp_err_t result;
                target_probe::TargetInfo *target;
                std::vector<hex_parser::Segment> *segments;
            };
            FlmCtx flm_ctx = { ESP_ERR_NOT_FOUND, &target, &g_flash_segments };

            usb_msc_volume::with_app_mount([](void *ctx) {
                auto *fc = static_cast<FlmCtx *>(ctx);
                std::string flm_path = usb_msc_volume::find_flm_file();
                if (flm_path.empty()) {
                    ESP_LOGW(kTag, "No FLM file found in /usb/algo/");
                    return;
                }
                ESP_LOGI(kTag, "Remote flash: FLM fallback with %s", flm_path.c_str());

                // Determine RAM size based on known devices
                constexpr uint32_t kDefaultRamBase = 0x20000000;
                constexpr uint32_t kDefaultRamSize = 8 * 1024;
                uint32_t ram_size = kDefaultRamSize;
                uint32_t dev = fc->target->dev_id;
                if (dev == 0x468 || dev == 0x469 || dev == 0x479) ram_size = 32 * 1024;
                else if (dev == 0x410 || dev == 0x414) ram_size = 20 * 1024;
                else if (dev == 0x413 || dev == 0x431 || dev == 0x441) ram_size = 64 * 1024;

                fc->result = flash_algo::program_with_flm(flm_path.c_str(), *fc->target,
                                                           *fc->segments, kDefaultRamBase, ram_size);
            }, &flm_ctx);

            err = flm_ctx.result;
        }

        if (err != ESP_OK) {
            char msg[64];
            snprintf(msg, sizeof(msg), "program failed: %s", esp_err_to_name(err));
            ws_send_flash_status(kFlashStatusError, msg);
        } else {
            size_t total = 0;
            for (auto &s : g_flash_segments) total += s.data.size();
            char msg[64];
            snprintf(msg, sizeof(msg), "ok %u bytes", (unsigned)total);
            ws_send_flash_status(kFlashStatusOk, msg);
        }

        g_flash_in_progress = false;
        g_flash_segments.clear();
        break;
    }
    default:
        ws_send_flash_status(kFlashStatusError, "unknown cmd");
        break;
    }

    delete fm;
}

// ----- TCP Relay Client -----

void tcp_relay_send_raw(const uint8_t *data, size_t len) {
    if (g_tcp_relay_fd < 0 || !g_tcp_relay_paired || len == 0) return;
    // Length-prefixed: [len_hi, len_lo, data...]
    uint8_t hdr[2];
    hdr[0] = (len >> 8) & 0xFF;
    hdr[1] = len & 0xFF;
    send(g_tcp_relay_fd, hdr, 2, 0);
    send(g_tcp_relay_fd, data, len, 0);
}

bool tcp_relay_connect() {
    struct hostent *he = gethostbyname(g_tcp_relay_host);
    if (!he) {
        ESP_LOGE(kTag, "TCP relay: DNS failed for %s", g_tcp_relay_host);
        return false;
    }

    g_tcp_relay_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_tcp_relay_fd < 0) {
        ESP_LOGE(kTag, "TCP relay: socket() failed");
        return false;
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_tcp_relay_port);
    memcpy(&addr.sin_addr, he->h_addr, he->h_length);

    if (connect(g_tcp_relay_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(kTag, "TCP relay: connect() failed: %d", errno);
        close(g_tcp_relay_fd);
        g_tcp_relay_fd = -1;
        return false;
    }

    int opt = 1;
    setsockopt(g_tcp_relay_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    // Send registration: [role=0x01(device), code_len, code...]
    uint8_t code_len = strlen(g_device_code);
    uint8_t reg[2 + 16];
    reg[0] = 0x01;  // ROLE_DEVICE
    reg[1] = code_len;
    memcpy(reg + 2, g_device_code, code_len);
    send(g_tcp_relay_fd, reg, 2 + code_len, 0);

    g_tcp_relay_connected = true;
    g_tcp_relay_paired = false;
    g_tcp_rx_len = 0;

    // Set non-blocking after registration sent
    int flags = fcntl(g_tcp_relay_fd, F_GETFL, 0);
    fcntl(g_tcp_relay_fd, F_SETFL, flags | O_NONBLOCK);

    ESP_LOGI(kTag, "TCP relay connected to %s:%d, code=%s",
             g_tcp_relay_host, g_tcp_relay_port, g_device_code);
    return true;
}

void tcp_relay_disconnect() {
    if (g_tcp_relay_fd >= 0) {
        close(g_tcp_relay_fd);
        g_tcp_relay_fd = -1;
    }
    g_tcp_relay_connected = false;
    g_tcp_relay_paired = false;
    g_tcp_rx_len = 0;
    // Resume USB CDC UART reading
    if (g_serial_client_fd < 0) {
        usb_cdc_bridge::set_uart_paused(false);
    }
}

// Dispatch a channel-prefixed message received from TCP relay
void tcp_relay_dispatch(const uint8_t *data, int len) {
    if (len < 1) return;
    uint8_t channel = data[0];
    const uint8_t *payload = data + 1;
    int payload_len = len - 1;

    if (channel == kWsChannelDap && payload_len <= (int)kBufSize && g_dap_cmd_queue) {
        DapCmdMsg *msg = new(std::nothrow) DapCmdMsg();
        if (msg) {
            memcpy(msg->data, payload, payload_len);
            msg->len = payload_len;
            msg->from_udp = false;
            if (xQueueSend(g_dap_cmd_queue, &msg, 0) != pdTRUE) {
                delete msg;
            }
        }
    } else if (channel == kWsChannelSerial && payload_len <= (int)sizeof(g_ws_serial_buf)) {
        memcpy(g_ws_serial_buf, payload, payload_len);
        g_ws_serial_len = payload_len;
        g_ws_serial_ready = true;
    } else if (channel == kWsChannelRtt && payload_len <= (int)sizeof(g_ws_rtt_buf)) {
        memcpy(g_ws_rtt_buf, payload, payload_len);
        g_ws_rtt_len = payload_len;
        g_ws_rtt_ready = true;
    } else if (channel == kWsChannelFlash && payload_len >= 1 && g_flash_queue) {
        uint8_t cmd = payload[0];
        FlashMsg *fm = new FlashMsg();
        if (fm) {
            fm->cmd = cmd;
            if (cmd == kFlashCmdData && payload_len >= 5) {
                fm->address = payload[1] | (payload[2] << 8) |
                              (payload[3] << 16) | (payload[4] << 24);
                if (payload_len > 5) {
                    fm->data.assign(payload + 5, payload + payload_len);
                }
            }
            if (xQueueSend(g_flash_queue, &fm, 0) != pdTRUE) {
                delete fm;
            }
        }
    } else if (channel == kWsChannelPunch && payload_len >= 1) {
        punch_handle_relay_msg(payload, payload_len);
    }
}

void tcp_relay_poll() {
    if (g_tcp_relay_fd < 0) return;

    // Read available data
    int space = sizeof(g_tcp_rx_buf) - g_tcp_rx_len;
    if (space <= 0) {
        ESP_LOGW(kTag, "TCP relay: rx buffer full, resetting");
        g_tcp_rx_len = 0;
        return;
    }

    int n = recv(g_tcp_relay_fd, g_tcp_rx_buf + g_tcp_rx_len, space, 0);
    if (n > 0) {
        g_tcp_rx_len += n;
    } else if (n == 0) {
        ESP_LOGW(kTag, "TCP relay: server disconnected");
        tcp_relay_disconnect();
        return;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGW(kTag, "TCP relay: recv error %d", errno);
        tcp_relay_disconnect();
        return;
    }

    // Process buffered data
    while (g_tcp_rx_len > 0) {
        if (!g_tcp_relay_paired) {
            // Waiting for pairing response
            // [0x00] = registered (waiting)
            // [0x01, port_hi, port_lo, ip_len, ip...] = paired
            // [0x02] = peer disconnected
            uint8_t status = g_tcp_rx_buf[0];
            if (status == 0x00) {
                // Registered, waiting for peer
                ESP_LOGI(kTag, "TCP relay: registered, waiting for peer");
                memmove(g_tcp_rx_buf, g_tcp_rx_buf + 1, g_tcp_rx_len - 1);
                g_tcp_rx_len -= 1;
            } else if (status == 0x01) {
                // Paired: [0x01, port_hi, port_lo, ip_len, ip...]
                if (g_tcp_rx_len < 4) break;  // need more data
                int ip_len = g_tcp_rx_buf[3];
                // Validate ip_len (max IPv4 "xxx.xxx.xxx.xxx" = 15, IPv6 ≤ 45)
                if (ip_len > 45) {
                    ESP_LOGW(kTag, "TCP relay: bad ip_len=%d, resetting", ip_len);
                    g_tcp_rx_len = 0;
                    break;
                }
                int msg_len = 4 + ip_len;
                if (g_tcp_rx_len < msg_len) break;  // need more data

                char peer_ip[64] = {};
                if (ip_len < (int)sizeof(peer_ip)) {
                    memcpy(peer_ip, g_tcp_rx_buf + 4, ip_len);
                }
                int peer_port = (g_tcp_rx_buf[1] << 8) | g_tcp_rx_buf[2];
                ESP_LOGW(kTag, "TCP relay: PAIRED! Peer=%s:%d", peer_ip, peer_port);

                g_tcp_relay_paired = true;
                // Pause USB CDC UART reading so relay serial works
                usb_cdc_bridge::set_uart_paused(true);

                // Start NAT hole punch attempt
                if (peer_ip[0] != '\0') {
                    punch_start(peer_ip, peer_port);
                }

                memmove(g_tcp_rx_buf, g_tcp_rx_buf + msg_len, g_tcp_rx_len - msg_len);
                g_tcp_rx_len -= msg_len;
            } else if (status == 0x02) {
                // Peer disconnected
                ESP_LOGW(kTag, "TCP relay: peer disconnected");
                g_tcp_relay_paired = false;
                // Reset punch state for next pairing
                if (g_punch_udp_fd >= 0) {
                    close(g_punch_udp_fd);
                    g_punch_udp_fd = -1;
                }
                g_punch_state = PUNCH_IDLE;
                g_punch_dap_active = false;
                memmove(g_tcp_rx_buf, g_tcp_rx_buf + 1, g_tcp_rx_len - 1);
                g_tcp_rx_len -= 1;
                usb_cdc_bridge::set_uart_paused(false);
            } else {
                ESP_LOGW(kTag, "TCP relay: unknown status 0x%02x", status);
                tcp_relay_disconnect();
                return;
            }
        } else {
            // Paired: read length-prefixed messages [len_hi, len_lo, data...]
            if (g_tcp_rx_len < 2) break;
            int msg_len = (g_tcp_rx_buf[0] << 8) | g_tcp_rx_buf[1];
            if (msg_len <= 0 || msg_len > 4000) {
                ESP_LOGW(kTag, "TCP relay: bad msg_len=%d, resetting", msg_len);
                g_tcp_rx_len = 0;
                break;
            }
            int total = 2 + msg_len;
            if (g_tcp_rx_len < total) break;  // need more data

            // Check for peer disconnect notification (sent as length-prefixed [0x00, 0x01, 0x02])
            if (msg_len == 1 && g_tcp_rx_buf[2] == 0x02) {
                ESP_LOGW(kTag, "TCP relay: peer disconnected");
                g_tcp_relay_paired = false;
                if (g_punch_udp_fd >= 0) {
                    close(g_punch_udp_fd);
                    g_punch_udp_fd = -1;
                }
                g_punch_state = PUNCH_IDLE;
                g_punch_dap_active = false;
                memmove(g_tcp_rx_buf, g_tcp_rx_buf + total, g_tcp_rx_len - total);
                g_tcp_rx_len -= total;
                usb_cdc_bridge::set_uart_paused(false);
                continue;
            }

            tcp_relay_dispatch(g_tcp_rx_buf + 2, msg_len);

            memmove(g_tcp_rx_buf, g_tcp_rx_buf + total, g_tcp_rx_len - total);
            g_tcp_rx_len -= total;
        }
    }
}

// ----- NAT Hole Punch (STUN-assisted) -----

void punch_start(const char *peer_ip, uint16_t peer_port) {
    // Reset any previous punch attempt
    if (g_punch_udp_fd >= 0) {
        close(g_punch_udp_fd);
        g_punch_udp_fd = -1;
    }
    g_punch_state = PUNCH_IDLE;
    g_punch_dap_active = false;

    // Save peer's public address (from TCP pairing)
    g_punch_peer_addr.sin_family = AF_INET;
    g_punch_peer_addr.sin_port = htons(peer_port);
    inet_pton(AF_INET, peer_ip, &g_punch_peer_addr.sin_addr);

    // Resolve relay host for STUN server address
    struct hostent *he = gethostbyname(g_tcp_relay_host);
    if (!he) {
        ESP_LOGE(kTag, "Punch: DNS failed for STUN server %s", g_tcp_relay_host);
        return;
    }
    g_punch_stun_addr.sin_family = AF_INET;
    g_punch_stun_addr.sin_port = htons(kStunPort);
    memcpy(&g_punch_stun_addr.sin_addr, he->h_addr, he->h_length);

    // Create UDP socket
    g_punch_udp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_punch_udp_fd < 0) {
        ESP_LOGE(kTag, "Punch: socket() failed");
        return;
    }

    // Bind to any port
    struct sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    local.sin_addr.s_addr = INADDR_ANY;
    if (bind(g_punch_udp_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(kTag, "Punch: bind() failed");
        close(g_punch_udp_fd);
        g_punch_udp_fd = -1;
        return;
    }

    // Set non-blocking
    int flags = fcntl(g_punch_udp_fd, F_GETFL, 0);
    fcntl(g_punch_udp_fd, F_SETFL, flags | O_NONBLOCK);

    // Send STUN probe to discover our external UDP port
    sendto(g_punch_udp_fd, "STUN", 4, 0,
           (struct sockaddr *)&g_punch_stun_addr, sizeof(g_punch_stun_addr));

    g_punch_state = PUNCH_STUN_PROBING;
    g_punch_dap_active = false;
    g_punch_peer_udp_port = 0;
    g_punch_my_ext_port = 0;
    g_punch_stun_retries = 1;
    g_punch_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    g_punch_last_send_ms = g_punch_start_ms;

    ESP_LOGI(kTag, "Punch: started, STUN probe sent to %s:%d, peer=%s:%u",
             g_tcp_relay_host, kStunPort, peer_ip, peer_port);
}

void punch_handle_relay_msg(const uint8_t *payload, int len) {
    if (len < 1) return;
    if (payload[0] == kPunchMyPort && len >= 3) {
        g_punch_peer_udp_port = (payload[1] << 8) | payload[2];
        ESP_LOGI(kTag, "Punch: peer external UDP port = %u", g_punch_peer_udp_port);
        // Update peer address with the peer's EXTERNAL UDP port
        g_punch_peer_addr.sin_port = htons(g_punch_peer_udp_port);
        // If we already have our own external port, start punching
        if (g_punch_state == PUNCH_WAIT_PEER && g_punch_my_ext_port > 0) {
            g_punch_state = PUNCH_PUNCHING;
            ESP_LOGI(kTag, "Punch: both ports known, starting punch");
        }
    } else if (payload[0] == kPunchDirect) {
        ESP_LOGI(kTag, "Punch: peer confirmed direct mode");
        g_punch_dap_active = true;
    }
}

void punch_poll() {
    if (g_punch_state == PUNCH_IDLE || g_punch_state == PUNCH_FAILED ||
        g_punch_udp_fd < 0) return;

    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // Timeout check
    if (g_punch_state != PUNCH_SUCCESS && (now - g_punch_start_ms) > kPunchTimeoutMs) {
        ESP_LOGW(kTag, "Punch: timeout (state=%d), staying on relay", g_punch_state);
        g_punch_state = PUNCH_FAILED;
        close(g_punch_udp_fd);
        g_punch_udp_fd = -1;
        return;
    }

    // State-specific actions
    if (g_punch_state == PUNCH_STUN_PROBING) {
        // Retry STUN probe every 250ms
        if ((now - g_punch_last_send_ms) >= 250 && g_punch_stun_retries < kStunMaxRetries) {
            g_punch_stun_retries++;
            g_punch_last_send_ms = now;
            sendto(g_punch_udp_fd, "STUN", 4, 0,
                   (struct sockaddr *)&g_punch_stun_addr, sizeof(g_punch_stun_addr));
        }
    } else if (g_punch_state == PUNCH_PUNCHING) {
        // Send punch packets periodically
        if ((now - g_punch_last_send_ms) >= kPunchIntervalMs) {
            g_punch_last_send_ms = now;
            sendto(g_punch_udp_fd, kPunchMagic, strlen(kPunchMagic), 0,
                   (struct sockaddr *)&g_punch_peer_addr, sizeof(g_punch_peer_addr));
        }
    } else if (g_punch_state == PUNCH_SUCCESS) {
        // Keepalive every 3s
        if ((now - g_punch_last_send_ms) >= 3000) {
            g_punch_last_send_ms = now;
            sendto(g_punch_udp_fd, "KA", 2, 0,
                   (struct sockaddr *)&g_punch_direct_addr, sizeof(g_punch_direct_addr));
        }
    }

    // Check for incoming UDP
    struct sockaddr_in from = {};
    socklen_t from_len = sizeof(from);
    uint8_t buf[kBufSize + 1];  // must hold full DAP commands
    int n = recvfrom(g_punch_udp_fd, buf, sizeof(buf) - 1, 0,
                     (struct sockaddr *)&from, &from_len);
    if (n <= 0) return;

    // Check if this is a STUN response (from relay server)
    if (g_punch_state == PUNCH_STUN_PROBING && n >= 3 &&
        from.sin_addr.s_addr == g_punch_stun_addr.sin_addr.s_addr) {
        // STUN response: [port_hi, port_lo, ip_ascii...]
        g_punch_my_ext_port = (buf[0] << 8) | buf[1];
        char ext_ip[64] = {};
        int ip_len = n - 2;
        if (ip_len >= (int)sizeof(ext_ip)) ip_len = sizeof(ext_ip) - 1;
        memcpy(ext_ip, buf + 2, ip_len);
        ext_ip[ip_len] = '\0';
        ESP_LOGW(kTag, "Punch: STUN response - external %s:%u", ext_ip, g_punch_my_ext_port);

        // Send our EXTERNAL UDP port to peer via relay
        uint8_t msg[4] = { kWsChannelPunch, kPunchMyPort,
                            (uint8_t)(g_punch_my_ext_port >> 8),
                            (uint8_t)(g_punch_my_ext_port & 0xFF) };
        relay_send(msg, 4);

        // Transition state
        if (g_punch_peer_udp_port > 0) {
            g_punch_state = PUNCH_PUNCHING;
            ESP_LOGI(kTag, "Punch: both ports known, starting punch");
        } else {
            g_punch_state = PUNCH_WAIT_PEER;
            ESP_LOGI(kTag, "Punch: waiting for peer's external port");
        }
        return;
    }

    // Punch magic received
    if (g_punch_state != PUNCH_SUCCESS &&
        n == (int)strlen(kPunchMagic) && memcmp(buf, kPunchMagic, n) == 0) {
        g_punch_state = PUNCH_SUCCESS;
        g_punch_direct_addr = from;

        char ip[16];
        inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
        ESP_LOGW(kTag, "Punch: SUCCESS! Direct peer: %s:%d", ip, ntohs(from.sin_port));

        // Send ACK
        sendto(g_punch_udp_fd, kPunchAck, strlen(kPunchAck), 0,
               (struct sockaddr *)&from, sizeof(from));

        // Notify peer via relay
        uint8_t msg[2] = { kWsChannelPunch, kPunchDirect };
        relay_send(msg, 2);
        g_punch_dap_active = true;
        return;
    }

    if (n == (int)strlen(kPunchAck) && memcmp(buf, kPunchAck, n) == 0) {
        if (g_punch_state != PUNCH_SUCCESS) {
            g_punch_state = PUNCH_SUCCESS;
            g_punch_direct_addr = from;
            char ip[16];
            inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
            ESP_LOGW(kTag, "Punch: ACK received! Direct peer: %s:%d",
                     ip, ntohs(from.sin_port));
            g_punch_dap_active = true;
        }
        return;
    }

    // Direct UDP DAP command (after punch success)
    if (g_punch_dap_active && n >= 2) {
        uint8_t channel = buf[0];
        if (channel == kWsChannelDap && n - 1 <= (int)kBufSize && g_dap_cmd_queue) {
            DapCmdMsg *msg = new(std::nothrow) DapCmdMsg();
            if (msg) {
                memcpy(msg->data, buf + 1, n - 1);
                msg->len = n - 1;
                msg->from_udp = true;
                if (xQueueSend(g_dap_cmd_queue, &msg, 0) != pdTRUE) {
                    delete msg;
                }
            }
        }
    }
}

// Send DAP response via direct UDP (when punch is active)
void punch_send_dap_response(const uint8_t *data, size_t len) {
    if (g_punch_udp_fd < 0 || !g_punch_dap_active) return;
    uint8_t framed[kBufSize + 1];
    framed[0] = kWsChannelDap;
    if (len > kBufSize) len = kBufSize;
    memcpy(framed + 1, data, len);
    sendto(g_punch_udp_fd, framed, len + 1, 0,
           (struct sockaddr *)&g_punch_direct_addr, sizeof(g_punch_direct_addr));
}

void tcp_relay_start() {
    if (g_tcp_relay_host[0] == '\0') return;

    // Init DAP command queue
    if (!g_dap_cmd_queue) {
        g_dap_cmd_queue = xQueueCreate(kDapCmdQueueDepth, sizeof(DapCmdMsg *));
    }

    // Init flash command queue
    if (!g_flash_queue) {
        g_flash_queue = xQueueCreate(16, sizeof(FlashMsg *));
    }

    if (!tcp_relay_connect()) {
        ESP_LOGW(kTag, "TCP relay: initial connect failed, will retry");
    }
}

void tcp_relay_reconnect_if_needed() {
    if (!g_tcp_relay_mode || g_tcp_relay_host[0] == '\0') return;
    if (g_tcp_relay_fd >= 0) return;  // still connected

    static uint32_t last_attempt = 0;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (now - last_attempt < 5000) return;  // retry every 5s
    last_attempt = now;

    ESP_LOGI(kTag, "TCP relay: reconnecting...");
    tcp_relay_connect();
}

// ----- WiFi Debug Task -----

// ----- Serial TCP Bridge -----

bool serial_start_server()
{
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(kSerialTcpPort);

    g_serial_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_serial_listen_fd < 0) {
        ESP_LOGE(kTag, "serial socket() failed: %d", errno);
        return false;
    }

    int opt = 1;
    setsockopt(g_serial_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int flags = fcntl(g_serial_listen_fd, F_GETFL, 0);
    fcntl(g_serial_listen_fd, F_SETFL, flags | O_NONBLOCK);

    if (bind(g_serial_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(kTag, "serial bind() failed: %d", errno);
        close(g_serial_listen_fd);
        g_serial_listen_fd = -1;
        return false;
    }

    if (listen(g_serial_listen_fd, 1) < 0) {
        ESP_LOGE(kTag, "serial listen() failed: %d", errno);
        close(g_serial_listen_fd);
        g_serial_listen_fd = -1;
        return false;
    }

    ESP_LOGI(kTag, "Serial TCP bridge listening on port %d", kSerialTcpPort);
    return true;
}

void serial_poll_tcp()
{
    if (g_serial_listen_fd < 0) return;

    fd_set read_fds;
    FD_ZERO(&read_fds);
    int max_fd = g_serial_listen_fd;
    FD_SET(g_serial_listen_fd, &read_fds);

    if (g_serial_client_fd >= 0) {
        FD_SET(g_serial_client_fd, &read_fds);
        if (g_serial_client_fd > max_fd) max_fd = g_serial_client_fd;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = 0 }; // non-blocking poll
    int sel = select(max_fd + 1, &read_fds, nullptr, nullptr, &tv);

    // Accept new connections
    if (g_serial_client_fd < 0 && sel > 0 && FD_ISSET(g_serial_listen_fd, &read_fds)) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        g_serial_client_fd = accept(g_serial_listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (g_serial_client_fd >= 0) {
            char ip[16];
            inet_ntoa_r(client_addr.sin_addr, ip, sizeof(ip));
            ESP_LOGW(kTag, "Serial TCP client connected from %s", ip);

            int flags = fcntl(g_serial_client_fd, F_GETFL, 0);
            fcntl(g_serial_client_fd, F_SETFL, flags | O_NONBLOCK);

            int opt = 1;
            setsockopt(g_serial_client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

            // Pause USB CDC UART reading so we get the data
            usb_cdc_bridge::set_uart_paused(true);
        }
    }

    if (g_serial_client_fd < 0) return;

    // TCP → UART: receive data from TCP client and write to UART
    if (sel > 0 && FD_ISSET(g_serial_client_fd, &read_fds)) {
        int n = recv(g_serial_client_fd, g_serial_buf, sizeof(g_serial_buf), 0);
        if (n > 0) {
            uart_write_bytes(board_config::kBridgeUart, g_serial_buf, n);
        } else if (n == 0) {
            ESP_LOGW(kTag, "Serial TCP client disconnected");
            close(g_serial_client_fd);
            g_serial_client_fd = -1;
            usb_cdc_bridge::set_uart_paused(false);
            return;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(kTag, "Serial TCP recv error: %d", errno);
            close(g_serial_client_fd);
            g_serial_client_fd = -1;
            usb_cdc_bridge::set_uart_paused(false);
            return;
        }
    }

    // UART → TCP: read from UART and send to TCP client
    int uart_avail = 0;
    uart_get_buffered_data_len(board_config::kBridgeUart, (size_t *)&uart_avail);
    if (uart_avail > 0) {
        int read = uart_read_bytes(board_config::kBridgeUart, g_serial_buf,
                                   sizeof(g_serial_buf), 0);
        if (read > 0) {
            send(g_serial_client_fd, g_serial_buf, read, 0);

            // Also forward to relay if connected
            if (relay_is_paired()) {
                uint8_t framed[257];
                framed[0] = kWsChannelSerial;
                memcpy(framed + 1, g_serial_buf, read);
                relay_send(framed, read + 1);
            }
        }
    }

    // Relay → UART: forward serial data from relay to UART and TCP client
    if (g_ws_serial_ready) {
        g_ws_serial_ready = false;
        uart_write_bytes(board_config::kBridgeUart, g_ws_serial_buf, g_ws_serial_len);
        // Echo back to local TCP client too
        if (g_serial_client_fd >= 0) {
            send(g_serial_client_fd, g_ws_serial_buf, g_ws_serial_len, 0);
        }
    }
}

// ----- Serial via Relay Only (no local TCP client) -----

void serial_poll_relay_only()
{
    // When no local serial TCP client but relay is paired,
    // still bridge UART ↔ relay serial channel
    if (g_serial_client_fd >= 0) return;  // local client handles it
    if (!relay_is_paired()) return;

    // UART → relay
    int uart_avail = 0;
    uart_get_buffered_data_len(board_config::kBridgeUart, (size_t *)&uart_avail);
    if (uart_avail > 0) {
        int read = uart_read_bytes(board_config::kBridgeUart, g_serial_buf,
                                   sizeof(g_serial_buf), 0);
        if (read > 0) {
            uint8_t framed[257];
            framed[0] = kWsChannelSerial;
            memcpy(framed + 1, g_serial_buf, read);
            relay_send(framed, read + 1);
        }
    }

    // Relay → UART
    if (g_ws_serial_ready) {
        g_ws_serial_ready = false;
        uart_write_bytes(board_config::kBridgeUart, g_ws_serial_buf, g_ws_serial_len);
    }
}

// ----- RTT TCP Server -----

bool rtt_start_server()
{
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(kRttTcpPort);

    g_rtt_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_rtt_listen_fd < 0) return false;

    int opt = 1;
    setsockopt(g_rtt_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    int flags = fcntl(g_rtt_listen_fd, F_GETFL, 0);
    fcntl(g_rtt_listen_fd, F_SETFL, flags | O_NONBLOCK);

    if (bind(g_rtt_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(g_rtt_listen_fd, 1) < 0) {
        close(g_rtt_listen_fd);
        g_rtt_listen_fd = -1;
        return false;
    }

    ESP_LOGI(kTag, "RTT TCP server listening on port %d", kRttTcpPort);
    return true;
}

void rtt_poll_tcp()
{
    if (g_rtt_listen_fd < 0) return;

    // Accept new connection if none active
    if (g_rtt_client_fd < 0) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        g_rtt_client_fd = accept(g_rtt_listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (g_rtt_client_fd >= 0) {
            char ip[16];
            inet_ntoa_r(client_addr.sin_addr, ip, sizeof(ip));
            ESP_LOGW(kTag, "RTT TCP client connected from %s", ip);
            int flags = fcntl(g_rtt_client_fd, F_GETFL, 0);
            fcntl(g_rtt_client_fd, F_SETFL, flags | O_NONBLOCK);
            int opt = 1;
            setsockopt(g_rtt_client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        }
    }

    if (g_rtt_client_fd < 0) return;

    // Read from TCP → RTT down channel
    uint8_t buf[128];
    int n = recv(g_rtt_client_fd, buf, sizeof(buf), 0);
    if (n > 0) {
        // Data will be consumed by rtt_recv()
        // Store for later consumption
        if (!g_ws_rtt_ready && n <= (int)sizeof(g_ws_rtt_buf)) {
            memcpy(g_ws_rtt_buf, buf, n);
            g_ws_rtt_len = n;
            g_ws_rtt_ready = true;
        }
    } else if (n == 0) {
        ESP_LOGW(kTag, "RTT TCP client disconnected");
        close(g_rtt_client_fd);
        g_rtt_client_fd = -1;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        close(g_rtt_client_fd);
        g_rtt_client_fd = -1;
    }
}

void wifi_debug_task(void *arg)
{
    // Wait for WiFi connection
    xEventGroupWaitBits(g_wifi_events, kWifiConnectedBit,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    // Brief delay for network stack to fully stabilize after WiFi connect
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Start TCP server
    el_start_server();

    // Start Serial TCP bridge
    serial_start_server();

    // Start RTT TCP server
    rtt_start_server();

    // Start WebSocket or TCP relay if configured
    if (g_tcp_relay_mode) {
        tcp_relay_start();
    } else {
        ws_start_relay();
    }

    // wifi_debug runs on Core 1 at priority 5. IDLE1 (priority 0) is
    // monitored by the task WDT (5s timeout). With CONFIG_FREERTOS_HZ=100,
    // vTaskDelay(pdMS_TO_TICKS(1)) = vTaskDelay(0) = taskYIELD which does
    // NOT yield to lower-priority tasks. We must periodically call
    // vTaskDelay(1) (1 tick = 10ms) to let IDLE1 run and feed the WDT.
    uint32_t last_idle_yield_tick = xTaskGetTickCount();

    while (true) {
        if (g_tcp_relay_mode) {
            tcp_relay_poll();
            tcp_relay_reconnect_if_needed();
        }
        ws_poll_relay();  // Process relay commands first (lowest latency)
        ws_poll_flash();  // Process remote flash commands
        punch_poll();     // NAT hole punch
        el_poll_tcp();
        serial_poll_tcp();
        serial_poll_relay_only();
        rtt_poll_tcp();

        // Yield to IDLE1 every ~2s so it can feed the task WDT (5s timeout).
        // vTaskDelay(1) = 1 tick = 10ms pause. Overhead: 10ms/2000ms = 0.5%.
        uint32_t now_tick = xTaskGetTickCount();
        if ((now_tick - last_idle_yield_tick) >= pdMS_TO_TICKS(2000)) {
            vTaskDelay(1);
            last_idle_yield_tick = xTaskGetTickCount();
        }

        vTaskDelay(pdMS_TO_TICKS(1));  // 1ms polling for responsive relay debugging
    }
}

} // anonymous namespace

// ----- Public API -----

namespace wifi_debug {

esp_err_t init()
{
    // Temporarily switch MSC to APP mode to read wifi.txt
    usb_msc_volume::with_app_mount(read_wifi_config_cb, nullptr);
    if (!g_config_read_ok) {
        return ESP_OK;  // Not configured, silently skip
    }
    g_wifi_configured = true;

    // Generate device code early (only needs MAC, not WiFi connection)
    generate_device_code();

    esp_err_t ret = init_wifi();
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "WiFi init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create dedicated task (needs stack for TCP + WS + CMSIS-DAP processing)
    xTaskCreatePinnedToCore(wifi_debug_task, "wifi_debug", 12288, nullptr,
                            5, nullptr, 1);  // Core 1, priority 5

    return ESP_OK;
}

void poll()
{
    // WiFi debug runs in its own task, nothing to do here
}

bool is_connected()
{
    return g_wifi_connected;
}

const char *get_device_code()
{
    return g_device_code;
}

const char *get_ip_str()
{
    return g_ip_str;
}

bool is_configured()
{
    return g_wifi_configured;
}

void rtt_send(const uint8_t *data, size_t length)
{
    if (length == 0) return;

    // Send to local TCP client
    if (g_rtt_client_fd >= 0) {
        send(g_rtt_client_fd, data, length, 0);
    }

    // Send to relay
    if (relay_is_paired()) {
        uint8_t framed[257];
        size_t to_send = length > 255 ? 255 : length;
        framed[0] = kWsChannelRtt;
        memcpy(framed + 1, data, to_send);
        relay_send(framed, to_send + 1);
    }
}

size_t rtt_recv(uint8_t *buffer, size_t buffer_size)
{
    if (!g_ws_rtt_ready) return 0;
    size_t len = (size_t)g_ws_rtt_len;
    if (len > buffer_size) len = buffer_size;
    memcpy(buffer, g_ws_rtt_buf, len);
    g_ws_rtt_ready = false;
    return len;
}

} // namespace wifi_debug
