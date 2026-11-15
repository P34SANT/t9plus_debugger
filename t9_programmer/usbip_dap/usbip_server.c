/*
 * Antminer T9+ Universal CMSIS-DAP USB/IP Debug Server
 * Direct Zynq-7000 EMIO GPIO SWD Engine for Windows & Linux IDEs
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/mman.h>

#include "usbip_proto.h"
#include "DAP_config.h"
#include "DAP.h"

#define TCP_SERV_PORT 3240
#define DAP_MAX_PACKET 512

#define DEBUG_VERBOSE 0
#ifdef DEBUG_VERBOSE
#define LOG_DAP(...) printf(__VA_ARGS__)
#else
#define LOG_DAP(...) do {} while (0)
#endif

volatile uint32_t *gpio_mem = NULL;
volatile uint32_t *swd_hw_mem = NULL;
bool swd_hw_available = false;

// Descriptors
static const USB_DEVICE_DESCRIPTOR dev_dsc = {
    .bLength            = sizeof(USB_DEVICE_DESCRIPTOR),
    .bDescriptorType    = USB_DESCRIPTOR_DEVICE,
    .bcdUSB             = 0x0210, // USB 2.10 (BOS supported)
    .bDeviceClass       = 0xEF,   // Miscellaneous Device Class (Composite IAD)
    .bDeviceSubClass    = 0x02,   // Common Class
    .bDeviceProtocol    = 0x01,   // Interface Association Descriptor
    .bMaxPacketSize0    = 64,
    .idVendor           = 0x0D28, // ARM Ltd.
    .idProduct          = 0x0204, // CMSIS-DAP
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,   // "Plink"
    .iProduct           = 0x02,   // "Plink Probe & Serial Port"
    .iSerialNumber      = 0x03,   // "0204000100000002"
    .bNumConfigurations = 0x01
};

static const USB_DEVICE_QUALIFIER_DESCRIPTOR dev_qua = {
    .bLength            = sizeof(USB_DEVICE_QUALIFIER_DESCRIPTOR),
    .bType              = USB_DESCRIPTOR_DEVICE_QUALIFIER,
    .bcdUSB             = 0x0210,
    .bDeviceClass       = 0xEF,
    .bDeviceSubClass    = 0x02,
    .bDeviceProtocol    = 0x01,
    .bMaxPacketSize0    = 64,
    .bNumConfigurations = 1,
    .bReserved          = 0
};

static const CONFIG_PLINK_COMPOSITE cfg_plink = {
    .config = {
        .bLength             = sizeof(USB_CONFIGURATION_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_CONFIGURATION,
        .wTotalLength        = sizeof(CONFIG_PLINK_COMPOSITE),
        .bNumInterfaces      = 5, // DAP + CDC0 (Comm+Data) + CDC1 (Comm+Data)
        .bConfigurationValue = 1,
        .iConfiguration      = 0,
        .bmAttributes        = 0x80, // Bus Powered
        .bMaxPower           = 250   // 500 mA
    },
    // Interface 0: WinUSB / CMSIS-DAP v2
    .intf_dap = {
        .bLength             = sizeof(USB_INTERFACE_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_INTERFACE,
        .bInterfaceNumber    = 0,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 2,
        .bInterfaceClass     = 0xFF, // Vendor Specific (WinUSB)
        .bInterfaceSubClass  = 0x00,
        .bInterfaceProtocol  = 0x00,
        .iInterface          = 4     // "Plink CMSIS-DAP v2"
    },
    .ep_dap_out = {
        .bLength             = sizeof(USB_ENDPOINT_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_ENDPOINT,
        .bEndpointAddress    = 0x01, // EP1 OUT
        .bmAttributes        = 0x02, // Bulk
        .wMaxPacketSize      = 512,
        .bInterval           = 0
    },
    .ep_dap_in = {
        .bLength             = sizeof(USB_ENDPOINT_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_ENDPOINT,
        .bEndpointAddress    = 0x81, // EP1 IN
        .bmAttributes        = 0x02, // Bulk
        .wMaxPacketSize      = 512,
        .bInterval           = 0
    },
    // IAD 1: CDC-ACM 0 (Primary Target UART0 -> J3 Pins 7/8)
    .iad_cdc0 = {
        .bLength             = sizeof(USB_IAD_DESCRIPTOR),
        .bDescriptorType     = 0x0B, // IAD
        .bFirstInterface     = 1,
        .bInterfaceCount     = 2,
        .bFunctionClass      = 0x02, // CDC
        .bFunctionSubClass   = 0x02, // ACM
        .bFunctionProtocol   = 0x01, // AT commands
        .iFunction           = 5     // "Plink UART0"
    },
    .intf_cdc0_comm = {
        .bLength             = sizeof(USB_INTERFACE_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_INTERFACE,
        .bInterfaceNumber    = 1,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0x02, // CDC Control
        .bInterfaceSubClass  = 0x02, // ACM
        .bInterfaceProtocol  = 0x01,
        .iInterface          = 5
    },
    .cdc0_header = {
        .bLength             = sizeof(USB_CDC_HEADER_DESCRIPTOR),
        .bDescriptorType     = 0x24,
        .bDescriptorSubtype  = 0x00, // Header
        .bcdCDC              = 0x0110
    },
    .cdc0_call_mgmt = {
        .bLength             = sizeof(USB_CDC_CALL_MGMT_DESCRIPTOR),
        .bDescriptorType     = 0x24,
        .bDescriptorSubtype  = 0x01,
        .bmCapabilities      = 0x00,
        .bDataInterface      = 2
    },
    .cdc0_acm = {
        .bLength             = sizeof(USB_CDC_ACM_DESCRIPTOR),
        .bDescriptorType     = 0x24,
        .bDescriptorSubtype  = 0x02,
        .bmCapabilities      = 0x02
    },
    .cdc0_union = {
        .bLength             = sizeof(USB_CDC_UNION_DESCRIPTOR),
        .bDescriptorType     = 0x24,
        .bDescriptorSubtype  = 0x06,
        .bControlInterface   = 1,
        .bSubordinateInterface0 = 2
    },
    .ep_cdc0_notify = {
        .bLength             = sizeof(USB_ENDPOINT_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_ENDPOINT,
        .bEndpointAddress    = 0x82, // EP2 IN
        .bmAttributes        = 0x03, // Interrupt
        .wMaxPacketSize      = 10,
        .bInterval           = 16
    },
    .intf_cdc0_data = {
        .bLength             = sizeof(USB_INTERFACE_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_INTERFACE,
        .bInterfaceNumber    = 2,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 2,
        .bInterfaceClass     = 0x0A, // CDC Data
        .bInterfaceSubClass  = 0x00,
        .bInterfaceProtocol  = 0x00,
        .iInterface          = 0
    },
    .ep_cdc0_out = {
        .bLength             = sizeof(USB_ENDPOINT_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_ENDPOINT,
        .bEndpointAddress    = 0x03, // EP3 OUT
        .bmAttributes        = 0x02, // Bulk
        .wMaxPacketSize      = 512,
        .bInterval           = 0
    },
    .ep_cdc0_in = {
        .bLength             = sizeof(USB_ENDPOINT_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_ENDPOINT,
        .bEndpointAddress    = 0x83, // EP3 IN
        .bmAttributes        = 0x02, // Bulk
        .wMaxPacketSize      = 512,
        .bInterval           = 0
    },

    // IAD 2: CDC-ACM 1 (Secondary Target UART1 -> J3 Pins 11/12)
    .iad_cdc1 = {
        .bLength             = sizeof(USB_IAD_DESCRIPTOR),
        .bDescriptorType     = 0x0B, // IAD
        .bFirstInterface     = 3,
        .bInterfaceCount     = 2,
        .bFunctionClass      = 0x02, // CDC
        .bFunctionSubClass   = 0x02, // ACM
        .bFunctionProtocol   = 0x01, // AT commands
        .iFunction           = 6     // "Plink UART1"
    },
    .intf_cdc1_comm = {
        .bLength             = sizeof(USB_INTERFACE_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_INTERFACE,
        .bInterfaceNumber    = 3,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 1,
        .bInterfaceClass     = 0x02, // CDC Control
        .bInterfaceSubClass  = 0x02, // ACM
        .bInterfaceProtocol  = 0x01,
        .iInterface          = 6
    },
    .cdc1_header = {
        .bLength             = sizeof(USB_CDC_HEADER_DESCRIPTOR),
        .bDescriptorType     = 0x24,
        .bDescriptorSubtype  = 0x00, // Header
        .bcdCDC              = 0x0110
    },
    .cdc1_call_mgmt = {
        .bLength             = sizeof(USB_CDC_CALL_MGMT_DESCRIPTOR),
        .bDescriptorType     = 0x24,
        .bDescriptorSubtype  = 0x01,
        .bmCapabilities      = 0x00,
        .bDataInterface      = 4
    },
    .cdc1_acm = {
        .bLength             = sizeof(USB_CDC_ACM_DESCRIPTOR),
        .bDescriptorType     = 0x24,
        .bDescriptorSubtype  = 0x02,
        .bmCapabilities      = 0x02
    },
    .cdc1_union = {
        .bLength             = sizeof(USB_CDC_UNION_DESCRIPTOR),
        .bDescriptorType     = 0x24,
        .bDescriptorSubtype  = 0x06,
        .bControlInterface   = 3,
        .bSubordinateInterface0 = 4
    },
    .ep_cdc1_notify = {
        .bLength             = sizeof(USB_ENDPOINT_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_ENDPOINT,
        .bEndpointAddress    = 0x84, // EP4 IN
        .bmAttributes        = 0x03, // Interrupt
        .wMaxPacketSize      = 10,
        .bInterval           = 16
    },
    .intf_cdc1_data = {
        .bLength             = sizeof(USB_INTERFACE_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_INTERFACE,
        .bInterfaceNumber    = 4,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 2,
        .bInterfaceClass     = 0x0A, // CDC Data
        .bInterfaceSubClass  = 0x00,
        .bInterfaceProtocol  = 0x00,
        .iInterface          = 0
    },
    .ep_cdc1_out = {
        .bLength             = sizeof(USB_ENDPOINT_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_ENDPOINT,
        .bEndpointAddress    = 0x05, // EP5 OUT
        .bmAttributes        = 0x02, // Bulk
        .wMaxPacketSize      = 512,
        .bInterval           = 0
    },
    .ep_cdc1_in = {
        .bLength             = sizeof(USB_ENDPOINT_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_ENDPOINT,
        .bEndpointAddress    = 0x85, // EP5 IN
        .bmAttributes        = 0x02, // Bulk
        .wMaxPacketSize      = 512,
        .bInterval           = 0
    }
};

// BOS Descriptor (Binary Device Object Store) - 33 bytes
static const uint8_t desc_bos[33] = {
    0x05,                               // bLength
    USB_DESCRIPTOR_BOS,                 // bDescriptorType
    0x21, 0x00,                         // wTotalLength (33)
    0x01,                               // bNumDeviceCaps

    // Microsoft OS 2.0 Platform Capability
    0x1C,                               // bLength
    USB_DESCRIPTOR_DEVICE_CAPABILITY,   // bDescriptorType
    0x05,                               // bDevCapabilityType (Platform)
    0x00,                               // bReserved
    // MS OS 2.0 Platform UUID: {D8DD60DF-4589-4CC7-9CD2-659D9E648A9F}
    0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
    0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
    0x00, 0x00, 0x03, 0x06,             // Windows version: Windows 8.1+ (0x06030000)
    0xB2, 0x00,                         // wMSOSDescriptorSetTotalLength (178)
    0x01,                               // bMS_VendorCode
    0x00                                // bAltEnumCode
};

// Microsoft OS 2.0 Descriptor Set (178 bytes) - Scoped to Interface 0 for WinUSB
static const uint8_t ms_os_20_desc[178] = {
    // Microsoft OS 2.0 Descriptor Set Header (10 bytes)
    0x0A, 0x00,                         // wLength
    0x00, 0x00,                         // MS_OS_20_SET_HEADER_DESCRIPTOR
    0x00, 0x00, 0x03, 0x06,             // dwWindowsVersion: Windows 8.1+
    0xB2, 0x00,                         // wTotalLength (178)

    // Configuration Subset Header (8 bytes)
    0x08, 0x00,                         // wLength
    0x01, 0x00,                         // MS_OS_20_SUBSET_HEADER_CONFIGURATION
    0x00,                               // bConfigurationValue
    0x00,                               // bReserved
    0xA8, 0x00,                         // wTotalLength (168)

    // Function Subset Header (8 bytes) - scopes WinUSB only to Interface 0 (CMSIS-DAP)
    0x08, 0x00,                         // wLength
    0x02, 0x00,                         // MS_OS_20_SUBSET_HEADER_FUNCTION
    0x00,                               // bFirstInterfaceNumber (0)
    0x00,                               // bReserved
    0xA0, 0x00,                         // wSubsetLength (160)

    // Compatible ID Descriptor (20 bytes)
    0x14, 0x00,                         // wLength
    0x03, 0x00,                         // MS_OS_20_FEATURE_COMPATIBLE_ID
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00, // CompatibleID ("WINUSB")
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // SubCompatibleID

    // Registry Property Descriptor (132 bytes)
    0x84, 0x00,                         // wLength
    0x04, 0x00,                         // MS_OS_20_FEATURE_REG_PROPERTY
    0x07, 0x00,                         // wPropertyDataType (REG_MULTI_SZ)
    0x2A, 0x00,                         // wPropertyNameLength (42 bytes)
    // "DeviceInterfaceGUIDs\0" in UTF-16LE
    'D',0,'e',0,'v',0,'i',0,'c',0,'e',0,'I',0,'n',0,'t',0,'e',0,'r',0,'f',0,'a',0,'c',0,'e',0,'G',0,'U',0,'I',0,'D',0,'s',0,0,0,
    0x50, 0x00,                         // wPropertyDataLength (80 bytes)
    // WinUSB CMSIS-DAP v2 GUID: "{CDB3B5AD-293B-4663-AA36-1AAE46463776}\0\0"
    '{',0,'C',0,'D',0,'B',0,'3',0,'B',0,'5',0,'A',0,'D',0,'-',0,
    '2',0,'9',0,'3',0,'B',0,'-',0,
    '4',0,'6',0,'6',0,'3',0,'-',0,
    'A',0,'A',0,'3',0,'6',0,'-',0,
    '1',0,'A',0,'A',0,'E',0,'4',0,'6',0,'4',0,'6',0,'3',0,'7',0,'7',0,'6',0,'}',0,
    0,0,0,0
};

static const char *strings[] = {
    "Plink",
    "Plink Probe & Dual Serial Ports",
    "0204000100000002",
    "Plink CMSIS-DAP v2",
    "Plink UART0 (Primary COM)",
    "Plink UART1 (Secondary COM)"
};

// Helper network I/O
static bool send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

static bool recv_all(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, MSG_WAITALL);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

static bool send_ret_submit(int fd, USBIP_CMD_SUBMIT *cmd, int status, const void *data, uint32_t len) {
    USBIP_RET_SUBMIT ret;
    memset(&ret, 0, sizeof(ret));
    ret.command       = htonl(0x0003);
    ret.seqnum        = htonl(cmd->seqnum);
    ret.devid         = htonl(cmd->devid);
    ret.direction     = htonl(cmd->direction);
    ret.ep            = htonl(cmd->ep);
    ret.status        = htonl(status);
    ret.actual_length = htonl(len);

    if (!send_all(fd, &ret, sizeof(ret))) return false;
    if (len > 0 && data != NULL) {
        if (!send_all(fd, data, len)) return false;
    }
    return true;
}

static bool handle_get_descriptor(int fd, USBIP_CMD_SUBMIT *cmd, setup_pkt_t *setup) {
    uint8_t desc_type = setup->wValue.hiByte;
    uint8_t desc_idx  = setup->wValue.lowByte;
    uint16_t req_len  = setup->wLength;

    switch (desc_type) {
    case USB_DESCRIPTOR_DEVICE: {
        uint32_t len = sizeof(dev_dsc);
        if (req_len < len) len = req_len;
        return send_ret_submit(fd, cmd, 0, &dev_dsc, len);
    }
    case USB_DESCRIPTOR_CONFIGURATION: {
        uint32_t len = sizeof(cfg_plink);
        if (req_len < len) len = req_len;
        return send_ret_submit(fd, cmd, 0, &cfg_plink, len);
    }
    case USB_DESCRIPTOR_DEVICE_QUALIFIER: {
        uint32_t len = sizeof(dev_qua);
        if (req_len < len) len = req_len;
        return send_ret_submit(fd, cmd, 0, &dev_qua, len);
    }
    case USB_DESCRIPTOR_BOS: {
        uint32_t len = sizeof(desc_bos);
        if (req_len < len) len = req_len;
        return send_ret_submit(fd, cmd, 0, desc_bos, len);
    }
    case USB_DESCRIPTOR_STRING: {
        if (desc_idx == 0) {
            uint8_t lang[4] = { 0x04, USB_DESCRIPTOR_STRING, 0x09, 0x04 };
            uint32_t len = (req_len < 4) ? req_len : 4;
            return send_ret_submit(fd, cmd, 0, lang, len);
        }
        if (desc_idx == 0xEE) {
            // Microsoft OS 1.0 String Descriptor: "MSFT100" with Vendor Code 0x02
            static const uint8_t msft100[18] = {
                0x12, USB_DESCRIPTOR_STRING,
                'M', 0, 'S', 0, 'F', 0, 'T', 0, '1', 0, '0', 0, '0', 0,
                0x02, 0x00
            };
            uint32_t len = (req_len < sizeof(msft100)) ? req_len : sizeof(msft100);
            return send_ret_submit(fd, cmd, 0, msft100, len);
        }
        if (desc_idx >= 1 && desc_idx <= (sizeof(strings) / sizeof(strings[0]))) {
            const char *str = strings[desc_idx - 1];
            size_t slen = strlen(str);
            uint8_t buf[256];
            buf[0] = (uint8_t)(slen * 2 + 2);
            buf[1] = USB_DESCRIPTOR_STRING;
            for (size_t i = 0; i < slen; i++) {
                buf[2 + i * 2]     = str[i];
                buf[2 + i * 2 + 1] = 0;
            }
            uint32_t total = slen * 2 + 2;
            if (req_len < total) total = req_len;
            return send_ret_submit(fd, cmd, 0, buf, total);
        }
        return send_ret_submit(fd, cmd, -1, NULL, 0);
    }
    default:
        return send_ret_submit(fd, cmd, -1, NULL, 0);
    }
}

#define MAX_PENDING_IN 16
static USBIP_CMD_SUBMIT pending_in_queue[MAX_PENDING_IN];
static int pending_in_head = 0;
static int pending_in_tail = 0;
static int pending_in_count = 0;

static void pending_in_init(void) {
    pending_in_head = 0;
    pending_in_tail = 0;
    pending_in_count = 0;
}

static bool pending_in_push(const USBIP_CMD_SUBMIT *cmd) {
    if (pending_in_count >= MAX_PENDING_IN) return false;
    pending_in_queue[pending_in_tail] = *cmd;
    pending_in_tail = (pending_in_tail + 1) % MAX_PENDING_IN;
    pending_in_count++;
    return true;
}

static bool pending_in_pop(USBIP_CMD_SUBMIT *out_cmd) {
    if (pending_in_count == 0) return false;
    *out_cmd = pending_in_queue[pending_in_head];
    pending_in_head = (pending_in_head + 1) % MAX_PENDING_IN;
    pending_in_count--;
    return true;
}

static bool pending_in_unlink(uint32_t seqnum, USBIP_CMD_SUBMIT *out_cmd) {
    for (int i = 0; i < pending_in_count; i++) {
        int idx = (pending_in_head + i) % MAX_PENDING_IN;
        if (pending_in_queue[idx].seqnum == seqnum) {
            if (out_cmd) *out_cmd = pending_in_queue[idx];
            for (int j = i; j < pending_in_count - 1; j++) {
                int cur = (pending_in_head + j) % MAX_PENDING_IN;
                int nxt = (pending_in_head + j + 1) % MAX_PENDING_IN;
                pending_in_queue[cur] = pending_in_queue[nxt];
            }
            pending_in_tail = (pending_in_tail - 1 + MAX_PENDING_IN) % MAX_PENDING_IN;
            pending_in_count--;
            return true;
        }
    }
    return false;
}

typedef struct __attribute__ ((__packed__)) {
    uint32_t dwDTERate;
    uint8_t  bCharFormat;
    uint8_t  bParityType;
    uint8_t  bDataBits;
} CDC_LINE_CODING;

static CDC_LINE_CODING cdc_line_coding = {
    .dwDTERate   = 115200,
    .bCharFormat = 0, // 1 stop bit
    .bParityType = 0, // None
    .bDataBits   = 8  // 8 bits
};

static bool handle_control_transfer(int fd, USBIP_CMD_SUBMIT *cmd) {
    setup_pkt_t *setup = (setup_pkt_t *)cmd->setup;

    if (setup->bmRequestType == 0x80) { // Device-to-Host Standard Device Request
        if (setup->bRequest == 0x06) { // GET_DESCRIPTOR
            return handle_get_descriptor(fd, cmd, setup);
        } else if (setup->bRequest == 0x00) { // GET_STATUS (Device)
            uint16_t status = 0;
            return send_ret_submit(fd, cmd, 0, &status, 2);
        }
    } else if (setup->bmRequestType == 0x81) { // Device-to-Host Standard Interface Request
        if (setup->bRequest == 0x00) { // GET_STATUS (Interface)
            uint16_t status = 0;
            return send_ret_submit(fd, cmd, 0, &status, 2);
        } else if (setup->bRequest == 0x0A) { // GET_INTERFACE
            uint8_t alt = 0;
            return send_ret_submit(fd, cmd, 0, &alt, 1);
        }
    } else if (setup->bmRequestType == 0x82) { // Device-to-Host Standard Endpoint Request
        if (setup->bRequest == 0x00) { // GET_STATUS (Endpoint)
            uint16_t status = 0; // Not halted
            return send_ret_submit(fd, cmd, 0, &status, 2);
        }
    } else if (setup->bmRequestType == 0x00) { // Host-to-Device Standard Device Request
        if (setup->bRequest == 0x09) { // SET_CONFIGURATION
            return send_ret_submit(fd, cmd, 0, NULL, 0);
        }
    } else if (setup->bmRequestType == 0x01) { // Host-to-Device Standard Interface Request
        if (setup->bRequest == 0x0B) { // SET_INTERFACE
            return send_ret_submit(fd, cmd, 0, NULL, 0);
        }
    } else if (setup->bmRequestType == 0x02) { // Host-to-Device Standard Endpoint Request
        if (setup->bRequest == 0x01) { // CLEAR_FEATURE (ENDPOINT_HALT)
            return send_ret_submit(fd, cmd, 0, NULL, 0);
        }
    } else if (setup->bmRequestType == 0x21) { // Host-to-Device Class Interface (CDC)
        if (setup->bRequest == 0x20) { // SET_LINE_CODING
            uint32_t len = setup->wLength;
            if (len > sizeof(cdc_line_coding)) len = sizeof(cdc_line_coding);
            recv_all(fd, &cdc_line_coding, len);
            return send_ret_submit(fd, cmd, 0, NULL, len);
        } else if (setup->bRequest == 0x22) { // SET_CONTROL_LINE_STATE
            return send_ret_submit(fd, cmd, 0, NULL, 0);
        }
    } else if (setup->bmRequestType == 0xA1) { // Device-to-Host Class Interface (CDC)
        if (setup->bRequest == 0x21) { // GET_LINE_CODING
            uint32_t len = setup->wLength;
            if (len > sizeof(cdc_line_coding)) len = sizeof(cdc_line_coding);
            return send_ret_submit(fd, cmd, 0, &cdc_line_coding, len);
        }
    } else if (setup->bmRequestType == 0xC0 || setup->bmRequestType == 0xC1) {
        // Vendor Device-to-Host Request: Microsoft OS 2.0 or OS 1.0
        if (setup->bRequest == 0x01) {
            // MS OS 2.0 Descriptor Set (Windows 8.1, 10, 11)
            uint32_t len = sizeof(ms_os_20_desc);
            if (setup->wLength < len) len = setup->wLength;
            return send_ret_submit(fd, cmd, 0, ms_os_20_desc, len);
        } else if (setup->bRequest == 0x02) {
            // MS OS 1.0 Feature Descriptors (Windows 7)
            if (setup->wIndex.W == 0x0004) {
                // Extended Compatible ID (40 bytes)
                static const uint8_t ms_os_10_compat_id[40] = {
                    0x28, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04, 0x00, 0x01, 0,0,0,0,0,0,0,
                    0x00, 0x01, 'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
                    0,0,0,0,0,0,0,0, 0,0,0,0,0,0
                };
                uint32_t len = sizeof(ms_os_10_compat_id);
                if (setup->wLength < len) len = setup->wLength;
                return send_ret_submit(fd, cmd, 0, ms_os_10_compat_id, len);
            } else if (setup->wIndex.W == 0x0005) {
                // Extended Properties - DeviceInterfaceGUID for WinUSB
                static const uint8_t ms_os_10_ext_prop[142] = {
                    0x8E, 0x00, 0x00, 0x00, 0x00, 0x01, 0x05, 0x00, 0x01, 0x00,
                    0x84, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x28, 0x00,
                    'D',0,'e',0,'v',0,'i',0,'c',0,'e',0,'I',0,'n',0,'t',0,'e',0,'r',0,'f',0,'a',0,'c',0,'e',0,'G',0,'U',0,'I',0,'D',0,0,0,
                    0x4E, 0x00, 0x00, 0x00,
                    '{',0,'C',0,'D',0,'B',0,'3',0,'B',0,'5',0,'A',0,'D',0,'-',0,
                    '2',0,'9',0,'3',0,'B',0,'-',0,
                    '4',0,'6',0,'6',0,'3',0,'-',0,
                    'A',0,'A',0,'3',0,'6',0,'-',0,
                    '1',0,'A',0,'A',0,'E',0,'4',0,'6',0,'4',0,'6',0,'3',0,'7',0,'7',0,'6',0,'}',0,
                    0,0
                };
                uint32_t len = sizeof(ms_os_10_ext_prop);
                if (setup->wLength < len) len = setup->wLength;
                return send_ret_submit(fd, cmd, 0, ms_os_10_ext_prop, len);
            }
        }
    }

    return send_ret_submit(fd, cmd, 0, NULL, 0);
}

static void handle_client(int client_fd) {
    int flag = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

    bool attached = false;
    uint8_t dap_req_buf[DAP_MAX_PACKET];
    uint8_t dap_resp_buf[DAP_MAX_PACKET];
    int dap_resp_len = 0;
    bool has_dap_resp = false;

    pending_in_init();

    int uart0_fd = -1;
    int uart1_fd = -1;

    while (1) {
        if (!attached) {
            uint16_t version, command;
            uint32_t status;
            if (!recv_all(client_fd, &version, 2)) break;
            if (!recv_all(client_fd, &command, 2)) break;
            if (!recv_all(client_fd, &status, 4)) break;

            command = ntohs(command);

            if (command == 0x8005) { // OP_REQ_DEVLIST
                OP_REP_DEVLIST_HEADER rep_hdr;
                rep_hdr.version = htons(0x0111);
                rep_hdr.command = htons(0x0005);
                rep_hdr.status  = htonl(0);
                rep_hdr.nExportedDevice = htonl(1);

                OP_REP_DEVLIST_DEVICE dev;
                memset(&dev, 0, sizeof(dev));
                snprintf(dev.usbPath, sizeof(dev.usbPath), "/sys/devices/platform/pcmsis/1-1");
                snprintf(dev.busID, sizeof(dev.busID), "1-1");
                dev.busnum = htonl(1);
                dev.devnum = htonl(2);
                dev.speed  = htonl(3); // USB_SPEED_HIGH (480 Mbps)
                dev.idVendor  = htons(dev_dsc.idVendor);
                dev.idProduct = htons(dev_dsc.idProduct);
                dev.bcdDevice = htons(dev_dsc.bcdDevice);
                dev.bDeviceClass = dev_dsc.bDeviceClass;
                dev.bDeviceSubClass = dev_dsc.bDeviceSubClass;
                dev.bDeviceProtocol = dev_dsc.bDeviceProtocol;
                dev.bConfigurationValue = 1;
                dev.bNumConfigurations = 1;
                dev.bNumInterfaces = 5;

                OP_REP_DEVLIST_INTERFACE intf[5];
                // Interface 0: WinUSB (CMSIS-DAP)
                intf[0].bInterfaceClass    = 0xFF;
                intf[0].bInterfaceSubClass = 0x00;
                intf[0].bInterfaceProtocol = 0x00;
                intf[0].padding            = 0;

                // Interface 1: CDC0 Comm (Primary UART0)
                intf[1].bInterfaceClass    = 0x02;
                intf[1].bInterfaceSubClass = 0x02;
                intf[1].bInterfaceProtocol = 0x01;
                intf[1].padding            = 0;

                // Interface 2: CDC0 Data
                intf[2].bInterfaceClass    = 0x0A;
                intf[2].bInterfaceSubClass = 0x00;
                intf[2].bInterfaceProtocol = 0x00;
                intf[2].padding            = 0;

                // Interface 3: CDC1 Comm (Secondary UART1)
                intf[3].bInterfaceClass    = 0x02;
                intf[3].bInterfaceSubClass = 0x02;
                intf[3].bInterfaceProtocol = 0x01;
                intf[3].padding            = 0;

                // Interface 4: CDC1 Data
                intf[4].bInterfaceClass    = 0x0A;
                intf[4].bInterfaceSubClass = 0x00;
                intf[4].bInterfaceProtocol = 0x00;
                intf[4].padding            = 0;

                send_all(client_fd, &rep_hdr, sizeof(rep_hdr));
                send_all(client_fd, &dev, sizeof(dev));
                send_all(client_fd, intf, sizeof(intf));
                break;
            } else if (command == 0x8003) { // OP_REQ_IMPORT
                char busid[32];
                if (!recv_all(client_fd, busid, 32)) break;

                OP_REP_IMPORT rep;
                memset(&rep, 0, sizeof(rep));
                rep.version = htons(0x0111);
                rep.command = htons(0x0003);
                rep.status  = htonl(0);
                snprintf(rep.usbPath, sizeof(rep.usbPath), "/sys/devices/platform/plink/1-1");
                snprintf(rep.busID, sizeof(rep.busID), "1-1");
                rep.busnum = htonl(1);
                rep.devnum = htonl(2);
                rep.speed  = htonl(3); // USB_SPEED_HIGH (480 Mbps)
                rep.idVendor  = htons(dev_dsc.idVendor);
                rep.idProduct = htons(dev_dsc.idProduct);
                rep.bcdDevice = htons(dev_dsc.bcdDevice);
                rep.bDeviceClass = dev_dsc.bDeviceClass;
                rep.bDeviceSubClass = dev_dsc.bDeviceSubClass;
                rep.bDeviceProtocol = dev_dsc.bDeviceProtocol;
                rep.bConfigurationValue = 1;
                rep.bNumConfigurations = 1;
                rep.bNumInterfaces = 5;

                if (!send_all(client_fd, &rep, sizeof(rep))) break;
                attached = true;
                pending_in_init();
                has_dap_resp = false;

                // Connect non-blocking socket to local UART0 TCP bridge (:2000)
                uart0_fd = socket(AF_INET, SOCK_STREAM, 0);
                if (uart0_fd >= 0) {
                    int flags = fcntl(uart0_fd, F_GETFL, 0);
                    fcntl(uart0_fd, F_SETFL, flags | O_NONBLOCK);
                    struct sockaddr_in uaddr;
                    memset(&uaddr, 0, sizeof(uaddr));
                    uaddr.sin_family = AF_INET;
                    uaddr.sin_port = htons(2000);
                    uaddr.sin_addr.s_addr = inet_addr("127.0.0.1");
                    connect(uart0_fd, (struct sockaddr *)&uaddr, sizeof(uaddr));
                }

                // Connect non-blocking socket to local UART1 TCP bridge (:2001)
                uart1_fd = socket(AF_INET, SOCK_STREAM, 0);
                if (uart1_fd >= 0) {
                    int flags = fcntl(uart1_fd, F_GETFL, 0);
                    fcntl(uart1_fd, F_SETFL, flags | O_NONBLOCK);
                    struct sockaddr_in uaddr;
                    memset(&uaddr, 0, sizeof(uaddr));
                    uaddr.sin_family = AF_INET;
                    uaddr.sin_port = htons(2001);
                    uaddr.sin_addr.s_addr = inet_addr("127.0.0.1");
                    connect(uart1_fd, (struct sockaddr *)&uaddr, sizeof(uaddr));
                }
                printf("[USBIP] Device 1-1 (Plink Composite: WinUSB + Dual CDC-ACM) imported. Connected to host.\n");
            } else {
                break;
            }
        } else {
            // Attached: Process USBIP URB Packets
            struct pollfd pfd = { .fd = client_fd, .events = POLLIN };
            int pr = poll(&pfd, 1, 100);
            if (pr == 0) {
                // Idle: no USB/IP packets received in 100ms -> turn D3 LEDs OFF
                dap_activity_idle();
                continue;
            } else if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }

            uint32_t cmd_code;
            if (!recv_all(client_fd, &cmd_code, 4)) {
                printf("[USBIP] recv cmd_code failed or connection closed: errno=%d (%s)\n", errno, strerror(errno));
                break;
            }
            cmd_code = ntohl(cmd_code);
            LOG_DAP("[USBIP] Got cmd_code: 0x%08X\n", cmd_code);

            if (cmd_code == 0x00000001) { // USBIP_CMD_SUBMIT
                USBIP_CMD_SUBMIT cmd;
                cmd.command = cmd_code;
                if (!recv_all(client_fd, ((uint32_t *)&cmd) + 1, sizeof(USBIP_CMD_SUBMIT) - 4)) {
                    printf("[USBIP] recv_all USBIP_CMD_SUBMIT rest failed!\n");
                    break;
                }

                cmd.seqnum                 = ntohl(cmd.seqnum);
                cmd.devid                  = ntohl(cmd.devid);
                cmd.direction              = ntohl(cmd.direction);
                cmd.ep                     = ntohl(cmd.ep);
                cmd.transfer_flags         = ntohl(cmd.transfer_flags);
                cmd.transfer_buffer_length = ntohl(cmd.transfer_buffer_length);
                cmd.start_frame            = ntohl(cmd.start_frame);
                cmd.number_of_packets      = ntohl(cmd.number_of_packets);
                cmd.interval               = ntohl(cmd.interval);

                if (cmd.ep == 0) {
                    LOG_DAP("[USBIP] EP0 control transfer req: bmReq=0x%02x bReq=0x%02x wVal=0x%04x wLen=%d\n",
                            ((setup_pkt_t *)cmd.setup)->bmRequestType,
                            ((setup_pkt_t *)cmd.setup)->bRequest,
                            ((setup_pkt_t *)cmd.setup)->wValue.W,
                            ((setup_pkt_t *)cmd.setup)->wLength);
                    if (!handle_control_transfer(client_fd, &cmd)) {
                        printf("[USBIP] handle_control_transfer failed!\n");
                        break;
                    }
                } else if (cmd.ep == 1) {
                    if (cmd.direction == 0) {
                        // EP1 OUT: Receive DAP command from host
                        uint32_t rlen = cmd.transfer_buffer_length;
                        LOG_DAP("[USBIP] EP1 OUT seq=%d len=%d\n", cmd.seqnum, rlen);
                        uint32_t to_read = (rlen > DAP_MAX_PACKET) ? DAP_MAX_PACKET : rlen;
                        if (!recv_all(client_fd, dap_req_buf, to_read)) {
                            printf("[USBIP] recv_all EP1 OUT payload failed!\n");
                            break;
                        }

                        // Drain excess bytes from TCP socket to maintain strict protocol framing
                        if (rlen > to_read) {
                            uint32_t excess = rlen - to_read;
                            uint8_t dummy[256];
                            while (excess > 0) {
                                uint32_t chunk = (excess > sizeof(dummy)) ? sizeof(dummy) : excess;
                                if (!recv_all(client_fd, dummy, chunk)) break;
                                excess -= chunk;
                            }
                        }

                        LOG_DAP("[USBIP] EP1 OUT DAP cmd=0x%02x\n", dap_req_buf[0]);

                        // Acknowledge OUT: actual_length must match rlen!
                        if (!send_ret_submit(client_fd, &cmd, 0, NULL, rlen)) {
                            printf("[USBIP] send_ret_submit EP1 OUT failed!\n");
                            break;
                        }

                        // Execute CMSIS-DAP command directly on Zynq SWD pins
                        dap_resp_len = dap_process_request(dap_req_buf, to_read, dap_resp_buf, DAP_MAX_PACKET);
                        LOG_DAP("[USBIP] DAP cmd executed -> resp_len=%d, resp[0]=0x%02x\n",
                                dap_resp_len, dap_resp_len > 0 ? dap_resp_buf[0] : 0);
                        has_dap_resp = true;

                        // If an IN URB is already pending, satisfy it immediately
                        if (pending_in_count > 0) {
                            USBIP_CMD_SUBMIT in_cmd;
                            pending_in_pop(&in_cmd);
                            uint32_t tx_len = (dap_resp_len > in_cmd.transfer_buffer_length) ? in_cmd.transfer_buffer_length : dap_resp_len;
                            LOG_DAP("[USBIP] Satisfying pending IN seq=%d with len=%d (remaining pending=%d)\n",
                                    in_cmd.seqnum, tx_len, pending_in_count);
                            if (!send_ret_submit(client_fd, &in_cmd, 0, dap_resp_buf, tx_len)) {
                                printf("[USBIP] send_ret_submit pending IN failed!\n");
                                break;
                            }
                            has_dap_resp = false;
                        } else {
                            has_dap_resp = true;
                        }
                    } else {
                        // EP1 IN: Host reading DAP response
                        LOG_DAP("[USBIP] EP1 IN seq=%d len=%d (has_resp=%d)\n",
                                cmd.seqnum, cmd.transfer_buffer_length, has_dap_resp);
                        if (has_dap_resp) {
                            uint32_t tx_len = (dap_resp_len > cmd.transfer_buffer_length) ? cmd.transfer_buffer_length : dap_resp_len;
                            if (!send_ret_submit(client_fd, &cmd, 0, dap_resp_buf, tx_len)) {
                                printf("[USBIP] send_ret_submit EP1 IN failed!\n");
                                break;
                            }
                            has_dap_resp = false;
                        } else {
                            if (!pending_in_push(&cmd)) {
                                printf("[USBIP] pending_in queue full! Overwriting oldest\n");
                                USBIP_CMD_SUBMIT dropped;
                                pending_in_pop(&dropped);
                                send_ret_submit(client_fd, &dropped, -ECONNRESET, NULL, 0);
                                pending_in_push(&cmd);
                            }
                        }
                    }
                } else if (cmd.ep == 2) {
                    // EP2 IN: CDC0 Notification (Interrupt)
                    static const uint8_t notify[10] = { 0xA1, 0x20, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00 };
                    uint32_t tx_len = (sizeof(notify) > cmd.transfer_buffer_length) ? cmd.transfer_buffer_length : sizeof(notify);
                    send_ret_submit(client_fd, &cmd, 0, notify, tx_len);
                } else if (cmd.ep == 3) {
                    // EP3: CDC0 Data (Primary Target UART0 -> J3 Pins 7/8)
                    if (cmd.direction == 0) { // OUT: Host writing to serial port
                        uint32_t rlen = cmd.transfer_buffer_length;
                        uint8_t ubuf[512];
                        uint32_t to_read = (rlen > sizeof(ubuf)) ? sizeof(ubuf) : rlen;
                        if (!recv_all(client_fd, ubuf, to_read)) break;
                        if (rlen > to_read) {
                            uint32_t excess = rlen - to_read;
                            uint8_t dummy[256];
                            while (excess > 0) {
                                uint32_t chunk = (excess > sizeof(dummy)) ? sizeof(dummy) : excess;
                                if (!recv_all(client_fd, dummy, chunk)) break;
                                excess -= chunk;
                            }
                        }
                        if (uart0_fd >= 0) {
                            send(uart0_fd, ubuf, to_read, MSG_NOSIGNAL);
                        }
                        send_ret_submit(client_fd, &cmd, 0, NULL, rlen);
                    } else { // IN: Host reading from serial port
                        uint8_t rx_buf[512];
                        ssize_t n_rx = 0;
                        if (uart0_fd >= 0) {
                            n_rx = recv(uart0_fd, rx_buf, sizeof(rx_buf), MSG_DONTWAIT);
                            if (n_rx < 0) n_rx = 0;
                        }
                        uint32_t tx_len = (uint32_t)n_rx;
                        if (tx_len > cmd.transfer_buffer_length) tx_len = cmd.transfer_buffer_length;
                        send_ret_submit(client_fd, &cmd, 0, (tx_len > 0) ? rx_buf : NULL, tx_len);
                    }
                } else if (cmd.ep == 4) {
                    // EP4 IN: CDC1 Notification (Interrupt)
                    static const uint8_t notify[10] = { 0xA1, 0x20, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00, 0x03, 0x00 };
                    uint32_t tx_len = (sizeof(notify) > cmd.transfer_buffer_length) ? cmd.transfer_buffer_length : sizeof(notify);
                    send_ret_submit(client_fd, &cmd, 0, notify, tx_len);
                } else if (cmd.ep == 5) {
                    // EP5: CDC1 Data (Secondary Target UART1 -> J3 Pins 11/12)
                    if (cmd.direction == 0) { // OUT: Host writing to serial port
                        uint32_t rlen = cmd.transfer_buffer_length;
                        uint8_t ubuf[512];
                        uint32_t to_read = (rlen > sizeof(ubuf)) ? sizeof(ubuf) : rlen;
                        if (!recv_all(client_fd, ubuf, to_read)) break;
                        if (rlen > to_read) {
                            uint32_t excess = rlen - to_read;
                            uint8_t dummy[256];
                            while (excess > 0) {
                                uint32_t chunk = (excess > sizeof(dummy)) ? sizeof(dummy) : excess;
                                if (!recv_all(client_fd, dummy, chunk)) break;
                                excess -= chunk;
                            }
                        }
                        if (uart1_fd >= 0) {
                            send(uart1_fd, ubuf, to_read, MSG_NOSIGNAL);
                        }
                        send_ret_submit(client_fd, &cmd, 0, NULL, rlen);
                    } else { // IN: Host reading from serial port
                        uint8_t rx_buf[512];
                        ssize_t n_rx = 0;
                        if (uart1_fd >= 0) {
                            n_rx = recv(uart1_fd, rx_buf, sizeof(rx_buf), MSG_DONTWAIT);
                            if (n_rx < 0) n_rx = 0;
                        }
                        uint32_t tx_len = (uint32_t)n_rx;
                        if (tx_len > cmd.transfer_buffer_length) tx_len = cmd.transfer_buffer_length;
                        send_ret_submit(client_fd, &cmd, 0, (tx_len > 0) ? rx_buf : NULL, tx_len);
                    }
                } else {
                    printf("[USBIP] Unknown ep %d\n", cmd.ep);
                    send_ret_submit(client_fd, &cmd, -1, NULL, 0);
                }
            } else if (cmd_code == 0x00000002) { // USBIP_CMD_UNLINK
                USBIP_CMD_UNLINK unl;
                unl.command = cmd_code;
                if (!recv_all(client_fd, ((uint32_t *)&unl) + 1, sizeof(USBIP_CMD_UNLINK) - 4)) {
                    printf("[USBIP] recv_all USBIP_CMD_UNLINK rest failed!\n");
                    break;
                }
                unl.seqnum     = ntohl(unl.seqnum);
                unl.seqnum_urb = ntohl(unl.seqnum_urb);
                LOG_DAP("[USBIP] CMD_UNLINK seq=%d target_seq=%d\n", unl.seqnum, unl.seqnum_urb);

                USBIP_RET_UNLINK ret_unl;
                memset(&ret_unl, 0, sizeof(ret_unl));
                ret_unl.command   = htonl(0x0004);
                ret_unl.seqnum    = htonl(unl.seqnum);
                ret_unl.devid     = 0;
                ret_unl.direction = 0;
                ret_unl.ep        = 0;

                USBIP_CMD_SUBMIT unlinked_cmd;
                if (pending_in_unlink(unl.seqnum_urb, &unlinked_cmd)) {
                    ret_unl.status = htonl(0);
                    send_all(client_fd, &ret_unl, sizeof(ret_unl));
                    send_ret_submit(client_fd, &unlinked_cmd, -ECONNRESET, NULL, 0);
                } else {
                    ret_unl.status = htonl(-ECONNRESET);
                    send_all(client_fd, &ret_unl, sizeof(ret_unl));
                }
            } else {
                printf("[USBIP] Unknown cmd_code: 0x%08X\n", cmd_code);
                break;
            }
        }
    }

    if (uart0_fd >= 0) close(uart0_fd);
    if (uart1_fd >= 0) close(uart1_fd);
    close(client_fd);
    DAP_CONFIG_DISCONNECT();
    dap_activity_idle();
    printf("[USBIP] Client disconnected.\n");
}

int main(void) {
    setlinebuf(stdout);
    setlinebuf(stderr);
    signal(SIGPIPE, SIG_IGN);

    printf("====================================================\n");
    printf(" Antminer T9+ Plink Debug & Serial Server\n");
    printf(" SWCLK: GPIO 54 | SWDIO: GPIO 55 | nRST: GPIO 56\n");
    printf("====================================================\n");

    if (zynq_gpio_init() < 0) {
        fprintf(stderr, "Failed to initialize Zynq GPIO registers!\n");
        return 1;
    }

    dap_init();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port        = htons(TCP_SERV_PORT);

    if (bind(server_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("[USBIP] Server listening on port %d...\n", TCP_SERV_PORT);
    printf("[USBIP] To attach on Windows:\n");
    printf("        usbip.exe attach -r <board_ip> -b 1-1\n\n");

    while (1) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli_addr.sin_addr, ip_str, sizeof(ip_str));
        printf("[USBIP] Connection accepted from %s:%d\n", ip_str, ntohs(cli_addr.sin_port));

        handle_client(client_fd);
    }

    close(server_fd);
    return 0;
}
