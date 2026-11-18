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

static volatile sig_atomic_t stop_requested;
static void request_stop(int signum) { (void)signum; stop_requested = 1; }
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
#include "../stlink/stlink_swd.h"
#include "../common/resource_lock.h"
#include "stream_ring.h"

#define TCP_SERV_PORT 3240
#define DAP_MAX_PACKET 512

static bool send_all(int fd, const void *buf, size_t len);
static bool recv_all(int fd, void *buf, size_t len);
static bool send_ret_submit(int fd, USBIP_CMD_SUBMIT *cmd, int status, const void *data, uint32_t len);

#define DEBUG_VERBOSE 0
#if DEBUG_VERBOSE
#define LOG_DAP(...) printf(__VA_ARGS__)
#else
#define LOG_DAP(...) do {} while (0)
#endif

volatile uint32_t *gpio_mem = NULL;
volatile uint32_t *swd_hw_mem = NULL;
volatile uint32_t *swo_hw_mem = NULL;
bool swd_hw_available = false;
bool swo_hw_available = false;

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

// Standard ST-LINK/V2 USB Descriptors (Pod #2 -> Header J4)
static const USB_DEVICE_DESCRIPTOR dev_dsc_stlink = {
    .bLength            = sizeof(USB_DEVICE_DESCRIPTOR),
    .bDescriptorType    = USB_DESCRIPTOR_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = 64,
    .idVendor           = 0x0483, // STMicroelectronics
    .idProduct          = 0x3748, // ST-LINK/V2
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,   // "STMicroelectronics"
    .iProduct           = 0x02,   // "STM32 STLink"
    .iSerialNumber      = 0x03,   // "066EFF535053717867204928"
    .bNumConfigurations = 0x01
};

typedef struct __attribute__ ((__packed__)) {
    USB_CONFIGURATION_DESCRIPTOR config;
    USB_INTERFACE_DESCRIPTOR     intf;
    USB_ENDPOINT_DESCRIPTOR      ep_in;  // EP1 IN (0x81) Bulk 512
    USB_ENDPOINT_DESCRIPTOR      ep_out; // EP2 OUT (0x02) Bulk 512
    USB_ENDPOINT_DESCRIPTOR      ep_swo; // EP3 IN (0x83) Bulk 512
} CONFIG_STLINK_V2;

static const CONFIG_STLINK_V2 cfg_stlink = {
    .config = {
        .bLength             = sizeof(USB_CONFIGURATION_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_CONFIGURATION,
        .wTotalLength        = sizeof(CONFIG_STLINK_V2),
        .bNumInterfaces      = 1,
        .bConfigurationValue = 1,
        .iConfiguration      = 0,
        .bmAttributes        = 0x80, // Bus Powered
        .bMaxPower           = 100   // 200 mA
    },
    .intf = {
        .bLength             = sizeof(USB_INTERFACE_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_INTERFACE,
        .bInterfaceNumber    = 0,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 3,
        .bInterfaceClass     = 0xFF, // Vendor Specific
        .bInterfaceSubClass  = 0xFF,
        .bInterfaceProtocol  = 0xFF,
        .iInterface          = 0
    },
    .ep_in = {
        .bLength             = sizeof(USB_ENDPOINT_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_ENDPOINT,
        .bEndpointAddress    = 0x81, // EP1 IN
        .bmAttributes        = 0x02, // Bulk
        .wMaxPacketSize      = 512,
        .bInterval           = 0
    },
    .ep_out = {
        .bLength             = sizeof(USB_ENDPOINT_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_ENDPOINT,
        .bEndpointAddress    = 0x02, // EP2 OUT
        .bmAttributes        = 0x02, // Bulk
        .wMaxPacketSize      = 512,
        .bInterval           = 0
    },
    .ep_swo = {
        .bLength             = sizeof(USB_ENDPOINT_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_ENDPOINT,
        .bEndpointAddress    = 0x83, // EP3 IN
        .bmAttributes        = 0x02, // Bulk
        .wMaxPacketSize      = 512,
        .bInterval           = 0
    }
};

static const char *strings_stlink[] = {
    "STMicroelectronics",
    "STM32 STLink",
    "066EFF535053717867204928"
};

static const uint8_t ms_compat_id_stlink[40] = {
    0x28, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04, 0x00, 0x01, 0,0,0,0,0,0,0,
    0x00, 0x01, 'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0
};

static const uint8_t ms_ext_prop_stlink[142] = {
    0x8E, 0x00, 0x00, 0x00, 0x00, 0x01, 0x05, 0x00, 0x01, 0x00,
    0x84, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x28, 0x00,
    'D',0,'e',0,'v',0,'i',0,'c',0,'e',0,'I',0,'n',0,'t',0,'e',0,'r',0,'f',0,'a',0,'c',0,'e',0,'G',0,'U',0,'I',0,'D',0,0,0,
    0x4E, 0x00, 0x00, 0x00,
    '{',0,'A',0,'5',0,'D',0,'C',0,'B',0,'F',0,'1',0,'0',0,'-',0,
    '6',0,'5',0,'3',0,'0',0,'-',0,
    '1',0,'1',0,'D',0,'2',0,'-',0,
    '9',0,'0',0,'1',0,'F',0,'-',0,
    '0',0,'0',0,'C',0,'0',0,'4',0,'F',0,'B',0,'9',0,'5',0,'1',0,'E',0,'D',0,'}',0,
    0,0
};

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
#define STLINK_CHECK(operation) do { if ((operation) != 0) { \
    resp_buf[0] = 0x80; resp_buf[1] = 0; return 2; } } while (0)
static int process_stlink_command(const uint8_t *cmd_buf, int cmd_len, uint8_t *resp_buf, int max_resp) {
    if (!cmd_buf || !resp_buf || cmd_len < 2 || max_resp < 8) return 0;
    uint8_t cmd0 = cmd_buf[0];
    uint8_t cmd1 = cmd_buf[1];
    if (cmd0 == 0xF2) {
        if (cmd1 == 0x07 || cmd1 == 0x32 || cmd1 == 0x08 || cmd1 == 0x33) {
            if (cmd_len < 8) return 0;
            unsigned count = cmd_buf[6] | ((unsigned)cmd_buf[7] << 8);
            if (cmd1 == 0x08 || cmd1 == 0x33) {
                if (count > (unsigned)(cmd_len - 8) / 4) return 0;
            } else if (count > (unsigned)max_resp / 4) return 0;
        }
        if (cmd1 == 0x34 && cmd_len < 3) return 0;
        if (cmd1 == 0x37 && cmd_len < 7) return 0;
    }

    if (cmd0 == 0xF1 && cmd1 == 0x80) { // STLINK_GET_VERSION
        resp_buf[0] = 0x20;
        resp_buf[1] = 0x00;
        resp_buf[2] = 0x07;
        resp_buf[3] = 0x02;
        resp_buf[4] = 0x83;
        resp_buf[5] = 0x04;
        return 6;
    } else if (cmd0 == 0xF5) { // STLINK_GET_CURRENT_MODE
        resp_buf[0] = 0x02; // Debug Mode
        resp_buf[1] = 0x00;
        return 2;
    } else if (cmd0 == 0xF2) { // STLINK_DEBUG_COMMAND
        switch (cmd1) {
        case 0x30: { // STLINK_DEBUG_ENTER (SWD)
            STLINK_CHECK(stlink_swd_init());
            STLINK_CHECK(stlink_swd_line_reset());
            resp_buf[0] = 0x00;
            resp_buf[1] = 0x00;
            return 2;
        }
        case 0x31: { // STLINK_DEBUG_EXIT
            stlink_swd_close();
            resp_buf[0] = 0x00;
            resp_buf[1] = 0x00;
            return 2;
        }
        case 0x22: { // STLINK_DEBUG_READCOREID
            uint32_t dpidr = 0;
            STLINK_CHECK(stlink_swd_read_dpidr(&dpidr));
            memcpy(resp_buf, &dpidr, 4);
            return 4;
        }
        case 0x03: { // STLINK_DEBUG_RESETSYS
            stlink_swd_reset_target(50);
            resp_buf[0] = 0x00;
            resp_buf[1] = 0x00;
            return 2;
        }
        case 0x07:   // STLINK_DEBUG_READMEM_32BIT
        case 0x32: {
            uint32_t addr = read_le32(cmd_buf + 2);
            uint16_t count = cmd_buf[6] | ((unsigned)cmd_buf[7] << 8);
            if (count * 4 > max_resp) count = max_resp / 4;
            uint32_t words[128];
            STLINK_CHECK(stlink_swd_read_mem32(addr, count, words));
            memcpy(resp_buf, words, count * 4);
            return count * 4;
        }
        case 0x08:   // STLINK_DEBUG_WRITEMEM_32BIT
        case 0x33: {
            uint32_t addr = read_le32(cmd_buf + 2);
            uint16_t count = cmd_buf[6] | ((unsigned)cmd_buf[7] << 8);
            uint32_t words[126];
            if (count > 126) return 0;
            for (unsigned i = 0; i < count; ++i) words[i] = read_le32(cmd_buf + 8 + i * 4);
            STLINK_CHECK(stlink_swd_write_mem32(addr, count, words));
            resp_buf[0] = 0x00;
            resp_buf[1] = 0x00;
            return 2;
        }
        case 0x35: { // STLINK_DEBUG_STEPCORE
            STLINK_CHECK(stlink_swd_step_core());
            resp_buf[0] = 0x00;
            resp_buf[1] = 0x00;
            return 2;
        }
        case 0x36: { // STLINK_DEBUG_RUNCORE
            STLINK_CHECK(stlink_swd_run_core());
            resp_buf[0] = 0x00;
            resp_buf[1] = 0x00;
            return 2;
        }
        case 0x3A: { // STLINK_DEBUG_GETSTATUS
            uint8_t status = 0;
            STLINK_CHECK(stlink_swd_get_status(&status));
            resp_buf[0] = status;
            resp_buf[1] = 0x00;
            return 2;
        }
        case 0x34: { // STLINK_DEBUG_READREG
            uint8_t reg_idx = cmd_buf[2];
            uint32_t val = 0;
            STLINK_CHECK(stlink_swd_read_core_reg(reg_idx, &val));
            memcpy(resp_buf, &val, 4);
            return 4;
        }
        case 0x37: { // STLINK_DEBUG_WRITEREG
            uint8_t reg_idx = cmd_buf[2];
            uint32_t val = read_le32(cmd_buf + 3);
            STLINK_CHECK(stlink_swd_write_core_reg(reg_idx, val));
            resp_buf[0] = 0x00;
            resp_buf[1] = 0x00;
            return 2;
        }
        case 0x40: { // STLINK_DEBUG_SYNC
            resp_buf[0] = 0x00;
            resp_buf[1] = 0x00;
            return 2;
        }
        default:
            resp_buf[0] = 0x80;
            resp_buf[1] = 0x00;
            return 2;
        }
    }

    resp_buf[0] = 0x80;
    resp_buf[1] = 0x00;
    return 2;
}

static bool receive_submit(int fd, USBIP_CMD_SUBMIT *cmd);
static void handle_stlink_session(int client_fd) {
    printf("[STLINK] Session active for Pod #2 (Header J4)...\n");
    uint8_t cmd_buf[512];
    uint8_t resp_buf[512];
    int resp_len = 0;
    bool has_resp = false;

    while (!stop_requested) {
        struct pollfd pfd = { .fd = client_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 100);
        if (pr == 0) continue;
        if (pr < 0) {
            if (errno == EINTR && !stop_requested) continue;
            break;
        }

        uint32_t cmd_code;
        if (!recv_all(client_fd, &cmd_code, 4)) break;
        cmd_code = ntohl(cmd_code);

        if (cmd_code == 0x00000001) { // USBIP_CMD_SUBMIT
            USBIP_CMD_SUBMIT cmd;
            if (!receive_submit(client_fd, &cmd) ||
                cmd.transfer_buffer_length > (int)sizeof(cmd_buf)) break;
            if (cmd.direction == 0 &&
                !recv_all(client_fd, cmd_buf, cmd.transfer_buffer_length)) break;

            if (cmd.ep == 0) {
                // Control transfer on ST-Link
                setup_pkt_t *setup = (setup_pkt_t *)cmd.setup;
                if (setup->bmRequestType == 0x80) {
                    if (setup->bRequest == 0x06) {
                        uint8_t dtype = setup->wValue.hiByte;
                        uint8_t didx  = setup->wValue.lowByte;
                        uint16_t req_l = setup->wLength;
                        if (dtype == USB_DESCRIPTOR_DEVICE) {
                            uint32_t len = (req_l < sizeof(dev_dsc_stlink)) ? req_l : sizeof(dev_dsc_stlink);
                            send_ret_submit(client_fd, &cmd, 0, &dev_dsc_stlink, len);
                        } else if (dtype == USB_DESCRIPTOR_CONFIGURATION) {
                            uint32_t len = (req_l < sizeof(cfg_stlink)) ? req_l : sizeof(cfg_stlink);
                            send_ret_submit(client_fd, &cmd, 0, &cfg_stlink, len);
                        } else if (dtype == USB_DESCRIPTOR_STRING) {
                            if (didx == 0) {
                                uint8_t lang[4] = { 0x04, USB_DESCRIPTOR_STRING, 0x09, 0x04 };
                                send_ret_submit(client_fd, &cmd, 0, lang, (req_l < 4) ? req_l : 4);
                            } else if (didx == 0xEE) {
                                static const uint8_t msft100[18] = { 0x12, USB_DESCRIPTOR_STRING, 'M',0,'S',0,'F',0,'T',0,'1',0,'0',0,'0',0, 0x02, 0 };
                                send_ret_submit(client_fd, &cmd, 0, msft100, (req_l < 18) ? req_l : 18);
                            } else if (didx <= 3) {
                                const char *str = strings_stlink[didx - 1];
                                size_t slen = strlen(str);
                                uint8_t buf[256];
                                buf[0] = (uint8_t)(slen * 2 + 2);
                                buf[1] = USB_DESCRIPTOR_STRING;
                                for (size_t i = 0; i < slen; i++) { buf[2 + i*2] = str[i]; buf[3 + i*2] = 0; }
                                uint32_t tot = slen * 2 + 2;
                                if (req_l < tot) tot = req_l;
                                send_ret_submit(client_fd, &cmd, 0, buf, tot);
                            } else {
                                send_ret_submit(client_fd, &cmd, -1, NULL, 0);
                            }
                        } else {
                            send_ret_submit(client_fd, &cmd, -1, NULL, 0);
                        }
                    } else if (setup->bRequest == 0x00) {
                        uint16_t st = 0;
                        send_ret_submit(client_fd, &cmd, 0, &st, 2);
                    } else {
                        send_ret_submit(client_fd, &cmd, 0, NULL, 0);
                    }
                } else if (setup->bmRequestType == 0xC0 || setup->bmRequestType == 0xC1) {
                    if (setup->bRequest == 0x02) {
                        if (setup->wIndex.W == 0x0004) {
                            send_ret_submit(client_fd, &cmd, 0, ms_compat_id_stlink, sizeof(ms_compat_id_stlink));
                        } else if (setup->wIndex.W == 0x0005) {
                            send_ret_submit(client_fd, &cmd, 0, ms_ext_prop_stlink, sizeof(ms_ext_prop_stlink));
                        } else {
                            send_ret_submit(client_fd, &cmd, 0, NULL, 0);
                        }
                    } else {
                        send_ret_submit(client_fd, &cmd, 0, NULL, 0);
                    }
                } else {
                    send_ret_submit(client_fd, &cmd, 0, NULL, 0);
                }
            } else if (cmd.ep == 1 || cmd.ep == 2) {
                if (cmd.direction == 0) { // OUT: Host sending ST-Link Command
                    uint32_t rlen = cmd.transfer_buffer_length;
                    if (has_resp) {
                        if (!send_ret_submit(client_fd, &cmd, -ENOSPC, NULL, 0)) break;
                        continue;
                    }
                    if (!send_ret_submit(client_fd, &cmd, 0, NULL, rlen)) break;

                    // Process ST-LINK Command
                    resp_len = process_stlink_command(cmd_buf, rlen, resp_buf, sizeof(resp_buf));
                    has_resp = true;
                } else { // IN: Host reading ST-Link response
                    if (has_resp) {
                        if (resp_len > cmd.transfer_buffer_length) {
                            if (!send_ret_submit(client_fd, &cmd, -EMSGSIZE, NULL, 0)) break;
                            continue;
                        }
                        if (!send_ret_submit(client_fd, &cmd, 0, resp_buf, resp_len)) break;
                        has_resp = false;
                    } else {
                        send_ret_submit(client_fd, &cmd, 0, NULL, 0);
                    }
                }
            } else if (cmd.ep == 3) {
                // EP3 IN: SWO Trace
                send_ret_submit(client_fd, &cmd, 0, NULL, 0);
            } else {
                send_ret_submit(client_fd, &cmd, -1, NULL, 0);
            }
        } else if (cmd_code == 0x00000002) { // UNLINK
            USBIP_CMD_UNLINK unl;
            unl.command = cmd_code;
            if (!recv_all(client_fd, ((uint32_t *)&unl) + 1, sizeof(USBIP_CMD_UNLINK) - 4)) break;
            USBIP_RET_UNLINK ret_unl;
            memset(&ret_unl, 0, sizeof(ret_unl));
            ret_unl.command = htonl(0x0004);
            ret_unl.seqnum  = unl.seqnum;
            ret_unl.status  = 0; /* This session has no deferred SUBMITs. */
            send_all(client_fd, &ret_unl, sizeof(ret_unl));
        } else {
            break;
        }
    }
    stlink_swd_close();
    printf("[STLINK] Session closed.\n");
}

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
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR && !stop_requested) continue;
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
        if (n < 0 && errno == EINTR && !stop_requested) continue;
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

static bool send_ret_submit(int fd, USBIP_CMD_SUBMIT *cmd, int status, const void *data, uint32_t len) {
    if (cmd->transfer_buffer_length < 0) return false;
    if (len > (uint32_t)cmd->transfer_buffer_length)
        len = (uint32_t)cmd->transfer_buffer_length;
    if (cmd->ep == 0 && cmd->direction == 1) {
        setup_pkt_t setup;
        memcpy(&setup, cmd->setup, sizeof(setup));
        if (len > setup.wLength) len = setup.wLength;
    }
    if (cmd->direction == 1 && len && !data) return false;
    USBIP_RET_SUBMIT ret;
    memset(&ret, 0, sizeof(ret));
    ret.command       = htonl(0x0003);
    ret.seqnum        = htonl(cmd->seqnum);
    /* Server response basic-header fields are reserved zero (USB/IP v1.1.1). */
    ret.status        = htonl(status);
    ret.actual_length = htonl(len);

    if (!send_all(fd, &ret, sizeof(ret))) return false;
    if (cmd->direction == 1 && len > 0 && data != NULL) {
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

#define MAX_PENDING_IN 64
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

static bool pending_in_unlink(uint32_t seqnum, USBIP_CMD_SUBMIT *out_cmd) {
    for (int i = 0; i < pending_in_count; i++) {
        int idx = (pending_in_head + i) % MAX_PENDING_IN;
        if ((uint32_t)pending_in_queue[idx].seqnum == seqnum) {
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

static bool pending_in_pop_ep(int ep, USBIP_CMD_SUBMIT *out)
{
    for (int i = 0; i < pending_in_count; ++i) {
        int idx = (pending_in_head + i) % MAX_PENDING_IN;
        if (pending_in_queue[idx].ep == ep)
            return pending_in_unlink(pending_in_queue[idx].seqnum, out);
    }
    return false;
}

typedef struct __attribute__ ((__packed__)) {
    uint32_t dwDTERate;
    uint8_t  bCharFormat;
    uint8_t  bParityType;
    uint8_t  bDataBits;
} CDC_LINE_CODING;

static CDC_LINE_CODING cdc_line_coding[2] = {
    {115200, 0, 0, 8}, {115200, 0, 0, 8}
};

static bool handle_control_transfer(int fd, USBIP_CMD_SUBMIT *cmd,
                                    const uint8_t *payload, size_t payload_length) {
    setup_pkt_t *setup = (setup_pkt_t *)cmd->setup;
    if (((setup->bmRequestType >> 7) & 1) != cmd->direction ||
        (cmd->direction == 0 && setup->wLength != payload_length))
        return send_ret_submit(fd, cmd, -EPIPE, NULL, 0);

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
            int port = setup->wIndex.W == 3;
            if ((setup->wIndex.W != 1 && setup->wIndex.W != 3) ||
                payload_length != sizeof(CDC_LINE_CODING))
                return send_ret_submit(fd, cmd, -EPIPE, NULL, 0);
            CDC_LINE_CODING coding;
            memcpy(&coding, payload, sizeof(coding));
            /* Both current backends are fixed at 115200 8N1. Never lie about baud. */
            if (coding.dwDTERate != 115200 || coding.bCharFormat ||
                coding.bParityType || coding.bDataBits != 8)
                return send_ret_submit(fd, cmd, -EPIPE, NULL, 0);
            cdc_line_coding[port] = coding;
            return send_ret_submit(fd, cmd, 0, NULL, payload_length);
        } else if (setup->bRequest == 0x22) { // SET_CONTROL_LINE_STATE
            return send_ret_submit(fd, cmd, 0, NULL, 0);
        }
    } else if (setup->bmRequestType == 0xA1) { // Device-to-Host Class Interface (CDC)
        if (setup->bRequest == 0x21) { // GET_LINE_CODING
            if (setup->wIndex.W != 1 && setup->wIndex.W != 3)
                return send_ret_submit(fd, cmd, -EPIPE, NULL, 0);
            uint32_t len = setup->wLength;
            if (len > sizeof(CDC_LINE_CODING)) len = sizeof(CDC_LINE_CODING);
            return send_ret_submit(fd, cmd, 0, &cdc_line_coding[setup->wIndex.W == 3], len);
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

    return send_ret_submit(fd, cmd, -EPIPE, NULL, 0);
}

#include "usbip_session.inc"

static void handle_client(int client_fd) {
    int flag = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

    int pod_lock = -1;
    struct timeval io_timeout = {.tv_sec = 5};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout));
    while (!stop_requested) {
            uint16_t version, command;
            uint32_t status;
            if (!recv_all(client_fd, &version, 2)) break;
            if (!recv_all(client_fd, &command, 2)) break;
            if (!recv_all(client_fd, &status, 4)) break;

            command = ntohs(command);
            if (ntohs(version) != 0x0111 || status != 0) break;

            if (command == 0x8005) { // OP_REQ_DEVLIST
                OP_REP_DEVLIST_HEADER rep_hdr;
                rep_hdr.version = htons(0x0111);
                rep_hdr.command = htons(0x0005);
                rep_hdr.status  = htonl(0);
                rep_hdr.nExportedDevice = htonl(2);

                // Device 1: Plink CMSIS-DAP v2 + Dual COM (Pod #1 -> Header J3)
                OP_REP_DEVLIST_DEVICE dev;
                memset(&dev, 0, sizeof(dev));
                snprintf(dev.usbPath, sizeof(dev.usbPath), "/sys/devices/platform/plink/1-1");
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

                // Device 2: STMicroelectronics ST-LINK/V2 (Pod #2 -> Header J4)
                OP_REP_DEVLIST_DEVICE dev2;
                memset(&dev2, 0, sizeof(dev2));
                snprintf(dev2.usbPath, sizeof(dev2.usbPath), "/sys/devices/platform/stlink/1-2");
                snprintf(dev2.busID, sizeof(dev2.busID), "1-2");
                dev2.busnum = htonl(1);
                dev2.devnum = htonl(3);
                dev2.speed  = htonl(3);
                dev2.idVendor  = htons(0x0483);
                dev2.idProduct = htons(0x3748);
                dev2.bcdDevice = htons(0x0100);
                dev2.bDeviceClass = 0x00;
                dev2.bDeviceSubClass = 0x00;
                dev2.bDeviceProtocol = 0x00;
                dev2.bConfigurationValue = 1;
                dev2.bNumConfigurations = 1;
                dev2.bNumInterfaces = 1;

                OP_REP_DEVLIST_INTERFACE intf2;
                intf2.bInterfaceClass    = 0xFF;
                intf2.bInterfaceSubClass = 0xFF;
                intf2.bInterfaceProtocol = 0xFF;
                intf2.padding            = 0;

                send_all(client_fd, &rep_hdr, sizeof(rep_hdr));
                send_all(client_fd, &dev, sizeof(dev));
                send_all(client_fd, intf, sizeof(intf));
                send_all(client_fd, &dev2, sizeof(dev2));
                send_all(client_fd, &intf2, sizeof(intf2));
                break;
            } else if (command == 0x8003) { // OP_REQ_IMPORT
                char busid[32];
                if (!recv_all(client_fd, busid, 32)) break;

                bool valid_busid = memchr(busid, 0, sizeof(busid)) &&
                                   (!strcmp(busid, "1-1") || !strcmp(busid, "1-2"));
                bool is_stlink = valid_busid && !strcmp(busid, "1-2");
                if (valid_busid)
                    pod_lock = plink_resource_lock(is_stlink ? "pod2" : "pod1");
                if (!valid_busid || pod_lock < 0 || !is_fpga_operating()) {
                    OP_REQ_DEVLIST rejected = {htons(0x0111), htons(3), htonl(1)};
                    send_all(client_fd, &rejected, sizeof(rejected));
                    break;
                }

                OP_REP_IMPORT rep;
                memset(&rep, 0, sizeof(rep));
                rep.version = htons(0x0111);
                rep.command = htons(0x0003);
                rep.status  = htonl(0);
                snprintf(rep.usbPath, sizeof(rep.usbPath), is_stlink ? "/sys/devices/platform/stlink/1-2" : "/sys/devices/platform/plink/1-1");
                snprintf(rep.busID, sizeof(rep.busID), "%s", busid);
                rep.busnum = htonl(1);
                rep.devnum = htonl(is_stlink ? 3 : 2);
                rep.speed  = htonl(3);
                rep.idVendor  = htons(is_stlink ? 0x0483 : dev_dsc.idVendor);
                rep.idProduct = htons(is_stlink ? 0x3748 : dev_dsc.idProduct);
                rep.bcdDevice = htons(0x0100);
                rep.bDeviceClass = is_stlink ? 0 : dev_dsc.bDeviceClass;
                rep.bDeviceSubClass = is_stlink ? 0 : dev_dsc.bDeviceSubClass;
                rep.bDeviceProtocol = is_stlink ? 0 : dev_dsc.bDeviceProtocol;
                rep.bConfigurationValue = 1;
                rep.bNumConfigurations = 1;
                rep.bNumInterfaces = is_stlink ? 1 : 5;

                if (!send_all(client_fd, &rep, sizeof(rep))) break;
                if (is_stlink) {
                    handle_stlink_session(client_fd);
                    break;
                }
                run_plink_session(client_fd);
                break;
            } else {
                break;
            }
    }

    close(client_fd);
    if (pod_lock >= 0) close(pod_lock);
    printf("[USBIP] Client disconnected.\n");
}

int main(void) {
    setlinebuf(stdout);
    setlinebuf(stderr);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    struct sigaction stop_action = {0};
    stop_action.sa_handler = request_stop;
    sigemptyset(&stop_action.sa_mask);
    sigaction(SIGTERM, &stop_action, NULL);
    sigaction(SIGINT, &stop_action, NULL);

    printf("====================================================\n");
    printf(" Antminer T9+ Plink Debug & Serial Server\n");
    printf(" SWCLK: GPIO 54 | SWDIO: GPIO 55 | nRST: GPIO 56\n");
    printf(" Pod #1: J3 (CMSIS-DAP + Dual COM) | Pod #2: J4 (ST-Link/V2)\n");
    printf("====================================================\n");

    /* Mapping/touching hardware is deferred until a successful exclusive import. */

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

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("[USBIP] Server listening on port %d...\n", TCP_SERV_PORT);
    printf("[USBIP] To attach on Windows:\n");
    printf("        usbip.exe attach -r <board_ip> -b 1-1  (Pod 1: CMSIS-DAP + COM9 + COM10)\n");
    printf("        usbip.exe attach -r <board_ip> -b 1-2  (Pod 2: ST-LINK/V2)\n\n");

    while (!stop_requested) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (client_fd < 0) {
            if (errno == EINTR && !stop_requested) continue;
            perror("accept");
            break;
        }

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli_addr.sin_addr, ip_str, sizeof(ip_str));
        printf("[USBIP] Connection accepted from %s:%d\n", ip_str, ntohs(cli_addr.sin_port));

        pid_t pid = fork();
        if (pid == 0) {
            close(server_fd);
            handle_client(client_fd);
            exit(0);
        }
        close(client_fd);
    }

    close(server_fd);
    return 0;
}
