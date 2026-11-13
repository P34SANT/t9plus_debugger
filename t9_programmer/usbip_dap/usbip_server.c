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
#include "dap_config.h"
#include "dap.h"

#define TCP_SERV_PORT 3240
#define DAP_MAX_PACKET 512

#ifdef DEBUG_VERBOSE
#define LOG_DAP(...) printf(__VA_ARGS__)
#else
#define LOG_DAP(...) do {} while (0)
#endif

volatile uint32_t *gpio_mem = NULL;

// Descriptors
static const USB_DEVICE_DESCRIPTOR dev_dsc = {
    .bLength            = sizeof(USB_DEVICE_DESCRIPTOR),
    .bDescriptorType    = USB_DESCRIPTOR_DEVICE,
    .bcdUSB             = 0x0210, // USB 2.10 (BOS supported)
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = 64,
    .idVendor           = 0x0D28, // ARM Ltd.
    .idProduct          = 0x0204, // CMSIS-DAP
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

static const USB_DEVICE_QUALIFIER_DESCRIPTOR dev_qua = {
    .bLength            = sizeof(USB_DEVICE_QUALIFIER_DESCRIPTOR),
    .bType              = USB_DESCRIPTOR_DEVICE_QUALIFIER,
    .bcdUSB             = 0x0210,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = 64,
    .bNumConfigurations = 1,
    .bReserved          = 0
};

static const CONFIG_CMSIS_DAP_V2 cfg_dap = {
    .dev_conf0 = {
        .bLength             = sizeof(USB_CONFIGURATION_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_CONFIGURATION,
        .wTotalLength        = sizeof(CONFIG_CMSIS_DAP_V2),
        .bNumInterfaces      = 1,
        .bConfigurationValue = 1,
        .iConfiguration      = 0,
        .bmAttributes        = 0x80, // Bus Powered
        .bMaxPower           = 250   // 500 mA
    },
    .dev_int0 = {
        .bLength             = sizeof(USB_INTERFACE_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_INTERFACE,
        .bInterfaceNumber    = 0,
        .bAlternateSetting   = 0,
        .bNumEndpoints       = 2,
        .bInterfaceClass     = 0xFF, // Vendor Specific (WinUSB)
        .bInterfaceSubClass  = 0x00,
        .bInterfaceProtocol  = 0x00,
        .iInterface          = 0
    },
    .ep_out = {
        .bLength             = sizeof(USB_ENDPOINT_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_ENDPOINT,
        .bEndpointAddress    = 0x01, // EP1 OUT
        .bmAttributes        = 0x02, // Bulk
        .wMaxPacketSize      = 512,
        .bInterval           = 0
    },
    .ep_in = {
        .bLength             = sizeof(USB_ENDPOINT_DESCRIPTOR),
        .bDescriptorType     = USB_DESCRIPTOR_ENDPOINT,
        .bEndpointAddress    = 0x81, // EP1 IN
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
    0xA2, 0x00,                         // wMSOSDescriptorSetTotalLength (162)
    0x01,                               // bMS_VendorCode
    0x00                                // bAltEnumCode
};

// Microsoft OS 2.0 Descriptor Set (162 bytes)
static const uint8_t ms_os_20_desc[162] = {
    // Microsoft OS 2.0 Descriptor Set Header (10 bytes)
    0x0A, 0x00,                         // wLength
    0x00, 0x00,                         // MS_OS_20_SET_HEADER_DESCRIPTOR
    0x00, 0x00, 0x03, 0x06,             // dwWindowsVersion: Windows 8.1+
    0xA2, 0x00,                         // wTotalLength (162)

    // Microsoft OS 2.0 Compatible ID Descriptor (20 bytes)
    0x14, 0x00,                         // wLength
    0x03, 0x00,                         // MS_OS_20_FEATURE_COMPATIBLE_ID
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00, // CompatibleID ("WINUSB")
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // SubCompatibleID

    // Microsoft OS 2.0 Registry Property Descriptor (132 bytes)
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
    "ARM",
    "CMSIS-DAP",
    "0204000100000000"
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
        uint32_t len = sizeof(cfg_dap);
        if (req_len < len) len = req_len;
        return send_ret_submit(fd, cmd, 0, &cfg_dap, len);
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
        if (desc_idx >= 1 && desc_idx <= 3) {
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

static bool pending_in_unlink(uint32_t seqnum) {
    for (int i = 0; i < pending_in_count; i++) {
        int idx = (pending_in_head + i) % MAX_PENDING_IN;
        if (pending_in_queue[idx].seqnum == seqnum) {
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
    } else if (setup->bmRequestType == 0xC0) {
        // Vendor Device-to-Host Request: Microsoft OS 2.0 Descriptor Set
        if (setup->bRequest == 0x01) {
            uint32_t len = sizeof(ms_os_20_desc);
            if (setup->wLength < len) len = setup->wLength;
            return send_ret_submit(fd, cmd, 0, ms_os_20_desc, len);
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
                snprintf(dev.usbPath, sizeof(dev.usbPath), "/sys/devices/platform/t9-dap/1-1");
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
                dev.bNumInterfaces = 1;

                OP_REP_DEVLIST_INTERFACE intf;
                intf.bInterfaceClass    = cfg_dap.dev_int0.bInterfaceClass;
                intf.bInterfaceSubClass = cfg_dap.dev_int0.bInterfaceSubClass;
                intf.bInterfaceProtocol = cfg_dap.dev_int0.bInterfaceProtocol;
                intf.padding = 0;

                send_all(client_fd, &rep_hdr, sizeof(rep_hdr));
                send_all(client_fd, &dev, sizeof(dev));
                send_all(client_fd, &intf, sizeof(intf));
                break;
            } else if (command == 0x8003) { // OP_REQ_IMPORT
                char busid[32];
                if (!recv_all(client_fd, busid, 32)) break;

                OP_REP_IMPORT rep;
                memset(&rep, 0, sizeof(rep));
                rep.version = htons(0x0111);
                rep.command = htons(0x0003);
                rep.status  = htonl(0);
                snprintf(rep.usbPath, sizeof(rep.usbPath), "/sys/devices/platform/t9-dap/1-1");
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
                rep.bNumInterfaces = 1;

                if (!send_all(client_fd, &rep, sizeof(rep))) break;
                attached = true;
                pending_in_init();
                has_dap_resp = false;
                printf("[USBIP] Device 1-1 imported. Connected to host.\n");
            } else {
                break;
            }
        } else {
            // Attached: Process USBIP URB Packets
            struct pollfd pfd = { .fd = client_fd, .events = POLLIN };
            int pr = poll(&pfd, 1, 100);
            if (pr == 0) {
                // Idle: no USB/IP packets received in 100ms -> turn D3 green OFF
                zynq_d3_led_set(0);
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
                        if (rlen > DAP_MAX_PACKET) rlen = DAP_MAX_PACKET;
                        if (!recv_all(client_fd, dap_req_buf, rlen)) {
                            printf("[USBIP] recv_all EP1 OUT payload failed!\n");
                            break;
                        }

                        LOG_DAP("[USBIP] EP1 OUT DAP cmd=0x%02x\n", dap_req_buf[0]);

                        // Acknowledge OUT: actual_length must match rlen!
                        if (!send_ret_submit(client_fd, &cmd, 0, NULL, rlen)) {
                            printf("[USBIP] send_ret_submit EP1 OUT failed!\n");
                            break;
                        }

                        // Execute CMSIS-DAP command directly on Zynq SWD pins
                        dap_resp_len = dap_process_request(dap_req_buf, rlen, dap_resp_buf, DAP_MAX_PACKET);
                        LOG_DAP("[USBIP] DAP cmd executed -> resp_len=%d, resp[0]=0x%02x\n",
                                dap_resp_len, dap_resp_len > 0 ? dap_resp_buf[0] : 0);
                        has_dap_resp = true;

                        // If an IN URB is already pending, satisfy it immediately
                        if (pending_in_count > 0) {
                            USBIP_CMD_SUBMIT in_cmd;
                            pending_in_pop(&in_cmd);
                            LOG_DAP("[USBIP] Satisfying pending IN seq=%d with len=%d (remaining pending=%d)\n",
                                    in_cmd.seqnum, dap_resp_len, pending_in_count);
                            if (!send_ret_submit(client_fd, &in_cmd, 0, dap_resp_buf, dap_resp_len)) {
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
                            if (!send_ret_submit(client_fd, &cmd, 0, dap_resp_buf, dap_resp_len)) {
                                printf("[USBIP] send_ret_submit EP1 IN failed!\n");
                                break;
                            }
                            has_dap_resp = false;
                        } else {
                            if (!pending_in_push(&cmd)) {
                                printf("[USBIP] pending_in queue full! Overwriting oldest\n");
                                USBIP_CMD_SUBMIT dropped;
                                pending_in_pop(&dropped);
                                pending_in_push(&cmd);
                            }
                        }
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

                if (pending_in_unlink(unl.seqnum_urb)) {
                    ret_unl.status = htonl(0);
                } else {
                    ret_unl.status = htonl(-ECONNRESET);
                }
                send_all(client_fd, &ret_unl, sizeof(ret_unl));
            } else {
                printf("[USBIP] Unknown cmd_code: 0x%08X\n", cmd_code);
                break;
            }
        }
    }

    close(client_fd);
    zynq_d3_led_set(0);
    printf("[USBIP] Client disconnected.\n");
}

int main(void) {
    setlinebuf(stdout);
    setlinebuf(stderr);
    signal(SIGPIPE, SIG_IGN);

    printf("====================================================\n");
    printf(" Antminer T9+ Universal CMSIS-DAP USB/IP Debug Server\n");
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
