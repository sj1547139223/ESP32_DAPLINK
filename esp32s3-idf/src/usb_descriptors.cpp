#include "usb_descriptors.h"

#include <cstring>

#include "board_config.h"
#include "cmsis_dap.h"
#include "esp_log.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

namespace {

enum InterfaceIndex : uint8_t {
    kItfCmsisDap = 0,
    kItfCdcComm,
    kItfCdcData,
    kItfMsc,
    kItfCount,
};

enum EndpointAddress : uint8_t {
    kEpCmsisDapOut = 0x01,
    kEpCmsisDapIn = 0x81,
    kEpCdcNotif = 0x82,
    kEpCdcOut = 0x03,
    kEpCdcIn = 0x83,
    kEpMscOut = 0x04,
    kEpMscIn = 0x84,
};

constexpr uint16_t kConfigTotalLength = TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN;
constexpr uint8_t kCdcStringIndex = 5;
constexpr uint8_t kMscStringIndex = 6;
constexpr uint8_t kVendorStringIndex = 4;

const tusb_desc_device_t kDeviceDescriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210,  // USB 2.1 required for BOS descriptor
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = board_config::kUsbVendorId,
    .idProduct = board_config::kUsbProductId,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

const uint8_t kConfigurationDescriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, kItfCount, 0, kConfigTotalLength, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_VENDOR_DESCRIPTOR(kItfCmsisDap, kVendorStringIndex, kEpCmsisDapOut, kEpCmsisDapIn, 64),
    TUD_CDC_DESCRIPTOR(kItfCdcComm, kCdcStringIndex, kEpCdcNotif, 8, kEpCdcOut, kEpCdcIn, 64),
    TUD_MSC_DESCRIPTOR(kItfMsc, kMscStringIndex, kEpMscOut, kEpMscIn, 64),
};

const char *kStringDescriptor[] = {
    (const char[]){0x09, 0x04},
    "ARM",
    "DAPLink CMSIS-DAP",
    "ESP32S3-0001",
    "CMSIS-DAP v2",
    "CDC Serial",
    "DAPLink MSC",
};

// ---- MS OS 2.0 Descriptors for WinUSB auto-install ----
constexpr uint8_t kVendorRequestMsOs20 = 0x01;

#define MS_OS_20_DESC_LEN 0xB2

// BOS Descriptor: announces MS OS 2.0 platform capability
const uint8_t kBosDescriptor[] = {
    // BOS header (5 bytes)
    TUD_BOS_DESCRIPTOR(5 + TUD_BOS_MICROSOFT_OS_DESC_LEN, 1),
    // MS OS 2.0 Platform Capability
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, kVendorRequestMsOs20),
};

// MS OS 2.0 Descriptor Set: binds WinUSB to the CMSIS-DAP vendor interface
const uint8_t kMsOs20DescriptorSet[] = {
    // MS OS 2.0 Set Header (10 bytes)
    0x0A, 0x00,                         // wLength
    0x00, 0x00,                         // MS_OS_20_SET_HEADER_DESCRIPTOR
    0x00, 0x00, 0x03, 0x06,            // dwWindowsVersion: Windows 8.1 (0x06030000)
    (MS_OS_20_DESC_LEN & 0xFF), ((MS_OS_20_DESC_LEN >> 8) & 0xFF),  // wTotalLength

    // MS OS 2.0 Configuration Subset Header (8 bytes)
    0x08, 0x00,                         // wLength
    0x01, 0x00,                         // MS_OS_20_SUBSET_HEADER_CONFIGURATION
    0x00,                               // bConfigurationValue
    0x00,                               // bReserved
    0xA8, 0x00,                         // wTotalLength of this subset (168 bytes)

    // MS OS 2.0 Function Subset Header (8 bytes) - for interface 0 (CMSIS-DAP)
    0x08, 0x00,                         // wLength
    0x02, 0x00,                         // MS_OS_20_SUBSET_HEADER_FUNCTION
    kItfCmsisDap,                       // bFirstInterface
    0x00,                               // bReserved
    0xA0, 0x00,                         // wSubsetLength (160 bytes)

    // MS OS 2.0 Compatible ID Descriptor (20 bytes) - WINUSB
    0x14, 0x00,                         // wLength
    0x03, 0x00,                         // MS_OS_20_FEATURE_COMPATBLE_ID
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,  // CompatibleID
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // SubCompatibleID

    // MS OS 2.0 Registry Property Descriptor (132 bytes) - DeviceInterfaceGUID
    0x84, 0x00,                         // wLength
    0x04, 0x00,                         // MS_OS_20_FEATURE_REG_PROPERTY
    0x07, 0x00,                         // wPropertyDataType: REG_MULTI_SZ
    0x2A, 0x00,                         // wPropertyNameLength (42 bytes)
    // PropertyName: "DeviceInterfaceGUIDs\0" in UTF-16LE
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00,
    'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00, 'r', 0x00, 'f', 0x00,
    'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00,
    'D', 0x00, 's', 0x00, 0x00, 0x00,
    0x50, 0x00,                         // wPropertyDataLength (80 bytes)
    // PropertyData: "{CDB3B5AD-293B-4663-AA36-1AAE46463776}\0\0" in UTF-16LE
    '{', 0x00, 'C', 0x00, 'D', 0x00, 'B', 0x00, '3', 0x00, 'B', 0x00,
    '5', 0x00, 'A', 0x00, 'D', 0x00, '-', 0x00, '2', 0x00, '9', 0x00,
    '3', 0x00, 'B', 0x00, '-', 0x00, '4', 0x00, '6', 0x00, '6', 0x00,
    '3', 0x00, '-', 0x00, 'A', 0x00, 'A', 0x00, '3', 0x00, '6', 0x00,
    '-', 0x00, '1', 0x00, 'A', 0x00, 'A', 0x00, 'E', 0x00, '4', 0x00,
    '6', 0x00, '4', 0x00, '6', 0x00, '3', 0x00, '7', 0x00, '7', 0x00,
    '6', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00,
};

const tinyusb_config_t kTinyUsbConfig = {
    .port = TINYUSB_PORT_FULL_SPEED_0,
    .phy = {
        .skip_setup = false,
        .self_powered = false,
        .vbus_monitor_io = -1,
    },
    .task = TINYUSB_TASK_DEFAULT(),
    .descriptor = {
        .device = &kDeviceDescriptor,
        .qualifier = nullptr,
        .string = kStringDescriptor,
        .string_count = sizeof(kStringDescriptor) / sizeof(kStringDescriptor[0]),
        .full_speed_config = kConfigurationDescriptor,
#if (TUD_OPT_HIGH_SPEED)
        .high_speed_config = kConfigurationDescriptor,
#else
        .high_speed_config = nullptr,
#endif
    },
    .event_cb = nullptr,
    .event_arg = nullptr,
};

} // namespace

namespace usb_descriptors {

const tinyusb_config_t *config()
{
    return &kTinyUsbConfig;
}

} // namespace usb_descriptors

// ---- TinyUSB BOS & Vendor Control Callbacks (for MS OS 2.0 / WinUSB) ----

extern "C" uint8_t const *tud_descriptor_bos_cb(void)
{
    return kBosDescriptor;
}

extern "C" bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                           tusb_control_request_t const *request)
{
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bRequest == kVendorRequestMsOs20 &&
        request->wIndex == 7) {
        // MS OS 2.0 Descriptor Set request
        if (stage == CONTROL_STAGE_SETUP) {
            uint16_t total_len = sizeof(kMsOs20DescriptorSet);
            uint16_t len = (request->wLength < total_len) ? request->wLength : total_len;
            return tud_control_xfer(rhport, request, (void *)(uintptr_t)kMsOs20DescriptorSet, len);
        }
        return true;
    }
    return false;
}
