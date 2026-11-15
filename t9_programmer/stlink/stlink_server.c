/*
 * stlink_server.c - High-Speed ST-LINK/V2 Hardware Debug Server for Plink Pod #2
 * Exports an authentic ST-LINK/V2 (0483:3748) via USB/IP on TCP port 3241
 * Targets: Header J4 (CLK: V18, DIO: V17, RST: Y16, SWO: T16)
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
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "stlink_swd.h"
#include "../usbip_dap/usbip_proto.h"

#define TCP_SERV_PORT 3241
#define STLINK_MAX_PACKET 512

// Standard ST-LINK/V2 USB Descriptors
static const USB_DEVICE_DESCRIPTOR dev_dsc = {
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
        .bEndpointAddress    = 0x83, // EP3 IN (SWO Trace)
        .bmAttributes        = 0x02, // Bulk
        .wMaxPacketSize      = 512,
        .bInterval           = 0
    }
};

static const char *strings[] = {
    "STMicroelectronics",
    "STM32 STLink",
    "066EFF535053717867204928"
};

// Microsoft OS 1.0 Extended Compatible ID Descriptor for WinUSB auto-binding
static const uint8_t ms_compat_id[40] = {
    0x28, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04, 0x00, 0x01, 0,0,0,0,0,0,0,
    0x00, 0x01, 'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0
};

static const uint8_t ms_ext_prop[142] = {
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
        uint32_t len = (req_len < sizeof(dev_dsc)) ? req_len : sizeof(dev_dsc);
        return send_ret_submit(fd, cmd, 0, &dev_dsc, len);
    }
    case USB_DESCRIPTOR_CONFIGURATION: {
        uint32_t len = (req_len < sizeof(cfg_stlink)) ? req_len : sizeof(cfg_stlink);
        return send_ret_submit(fd, cmd, 0, &cfg_stlink, len);
    }
    case USB_DESCRIPTOR_STRING: {
        if (desc_idx == 0) {
            uint8_t lang[4] = { 0x04, USB_DESCRIPTOR_STRING, 0x09, 0x04 };
            uint32_t len = (req_len < 4) ? req_len : 4;
            return send_ret_submit(fd, cmd, 0, lang, len);
        }
        if (desc_idx == 0xEE) {
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

static bool handle_control_transfer(int fd, USBIP_CMD_SUBMIT *cmd) {
    setup_pkt_t *setup = (setup_pkt_t *)cmd->setup;

    if (setup->bmRequestType == 0x80) {
        if (setup->bRequest == 0x06) return handle_get_descriptor(fd, cmd, setup);
        if (setup->bRequest == 0x00) {
            uint16_t status = 0;
            return send_ret_submit(fd, cmd, 0, &status, 2);
        }
    } else if (setup->bmRequestType == 0xC0 || setup->bmRequestType == 0xC1) {
        if (setup->bRequest == 0x02) {
            if (setup->wIndex.W == 0x0004) {
                uint32_t len = (setup->wLength < sizeof(ms_compat_id)) ? setup->wLength : sizeof(ms_compat_id);
                return send_ret_submit(fd, cmd, 0, ms_compat_id, len);
            } else if (setup->wIndex.W == 0x0005) {
                uint32_t len = (setup->wLength < sizeof(ms_ext_prop)) ? setup->wLength : sizeof(ms_ext_prop);
                return send_ret_submit(fd, cmd, 0, ms_ext_prop, len);
            }
        }
    }

    return send_ret_submit(fd, cmd, 0, NULL, 0);
}

// ST-LINK Command Processing
static int process_stlink_command(const uint8_t *cmd_buf, int cmd_len, uint8_t *resp_buf, int max_resp) {
    if (cmd_len < 2) return 0;
    uint8_t cmd0 = cmd_buf[0];
    uint8_t cmd1 = cmd_buf[1];

    if (cmd0 == 0xF1 && cmd1 == 0x80) { // STLINK_GET_VERSION
        // ST-LINK/V2, JTAG v29, SWIM v7, VID 0483
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
            stlink_swd_init();
            stlink_swd_line_reset();
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
            stlink_swd_read_dpidr(&dpidr);
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
            uint32_t addr = *(uint32_t *)&cmd_buf[2];
            uint16_t count = *(uint16_t *)&cmd_buf[6];
            if (count * 4 > max_resp) count = max_resp / 4;
            stlink_swd_read_mem32(addr, count, (uint32_t *)resp_buf);
            return count * 4;
        }
        case 0x08:   // STLINK_DEBUG_WRITEMEM_32BIT
        case 0x33: {
            uint32_t addr = *(uint32_t *)&cmd_buf[2];
            uint16_t count = *(uint16_t *)&cmd_buf[6];
            const uint32_t *data = (const uint32_t *)&cmd_buf[8];
            stlink_swd_write_mem32(addr, count, data);
            resp_buf[0] = 0x00;
            resp_buf[1] = 0x00;
            return 2;
        }
        case 0x35: { // STLINK_DEBUG_STEPCORE
            stlink_swd_step_core();
            resp_buf[0] = 0x00;
            resp_buf[1] = 0x00;
            return 2;
        }
        case 0x36: { // STLINK_DEBUG_RUNCORE
            stlink_swd_run_core();
            resp_buf[0] = 0x00;
            resp_buf[1] = 0x00;
            return 2;
        }
        case 0x3A: { // STLINK_DEBUG_GETSTATUS
            uint8_t status = 0;
            stlink_swd_get_status(&status);
            resp_buf[0] = status;
            resp_buf[1] = 0x00;
            return 2;
        }
        case 0x34: { // STLINK_DEBUG_READREG
            uint8_t reg_idx = cmd_buf[2];
            uint32_t val = 0;
            stlink_swd_read_core_reg(reg_idx, &val);
            memcpy(resp_buf, &val, 4);
            return 4;
        }
        case 0x37: { // STLINK_DEBUG_WRITEREG
            uint8_t reg_idx = cmd_buf[2];
            uint32_t val = *(uint32_t *)&cmd_buf[3];
            stlink_swd_write_core_reg(reg_idx, val);
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
            resp_buf[0] = 0x00;
            resp_buf[1] = 0x00;
            return 2;
        }
    }

    resp_buf[0] = 0x00;
    resp_buf[1] = 0x00;
    return 2;
}

static void handle_client(int client_fd) {
    int flag = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

    bool attached = false;
    uint8_t cmd_buf[STLINK_MAX_PACKET];
    uint8_t resp_buf[STLINK_MAX_PACKET];
    int resp_len = 0;
    bool has_resp = false;

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
                snprintf(dev.usbPath, sizeof(dev.usbPath), "/sys/devices/platform/stlink/1-1");
                snprintf(dev.busID, sizeof(dev.busID), "1-1");
                dev.busnum = htonl(1);
                dev.devnum = htonl(3);
                dev.speed  = htonl(3); // High-Speed
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
                intf.bInterfaceClass    = cfg_stlink.intf.bInterfaceClass;
                intf.bInterfaceSubClass = cfg_stlink.intf.bInterfaceSubClass;
                intf.bInterfaceProtocol = cfg_stlink.intf.bInterfaceProtocol;
                intf.padding            = 0;

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
                snprintf(rep.usbPath, sizeof(rep.usbPath), "/sys/devices/platform/stlink/1-1");
                snprintf(rep.busID, sizeof(rep.busID), "1-1");
                rep.busnum = htonl(1);
                rep.devnum = htonl(3);
                rep.speed  = htonl(3);
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
                printf("[STLINK] ST-Link/V2 Device imported. Connected to host.\n");
            } else {
                break;
            }
        } else {
            struct pollfd pfd = { .fd = client_fd, .events = POLLIN };
            int pr = poll(&pfd, 1, 100);
            if (pr == 0) continue;
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }

            uint32_t cmd_code;
            if (!recv_all(client_fd, &cmd_code, 4)) break;
            cmd_code = ntohl(cmd_code);

            if (cmd_code == 0x00000001) { // USBIP_CMD_SUBMIT
                USBIP_CMD_SUBMIT cmd;
                cmd.command = cmd_code;
                if (!recv_all(client_fd, ((uint32_t *)&cmd) + 1, sizeof(USBIP_CMD_SUBMIT) - 4)) break;
                cmd.seqnum                 = ntohl(cmd.seqnum);
                cmd.devid                  = ntohl(cmd.devid);
                cmd.direction              = ntohl(cmd.direction);
                cmd.ep                     = ntohl(cmd.ep);
                cmd.transfer_buffer_length = ntohl(cmd.transfer_buffer_length);

                if (cmd.ep == 0) {
                    handle_control_transfer(client_fd, &cmd);
                } else if (cmd.ep == 1 || cmd.ep == 2) {
                    if (cmd.direction == 0) { // OUT: Host sending ST-Link Command
                        uint32_t rlen = cmd.transfer_buffer_length;
                        uint32_t to_read = (rlen > sizeof(cmd_buf)) ? sizeof(cmd_buf) : rlen;
                        if (!recv_all(client_fd, cmd_buf, to_read)) break;
                        if (rlen > to_read) {
                            uint32_t excess = rlen - to_read;
                            uint8_t dummy[256];
                            while (excess > 0) {
                                uint32_t chk = (excess > sizeof(dummy)) ? sizeof(dummy) : excess;
                                if (!recv_all(client_fd, dummy, chk)) break;
                                excess -= chk;
                            }
                        }

                        send_ret_submit(client_fd, &cmd, 0, NULL, rlen);

                        // Process ST-LINK Command
                        resp_len = process_stlink_command(cmd_buf, to_read, resp_buf, sizeof(resp_buf));
                        has_resp = true;
                    } else { // IN: Host reading ST-Link response
                        if (has_resp) {
                            uint32_t tx_len = (resp_len > cmd.transfer_buffer_length) ? cmd.transfer_buffer_length : resp_len;
                            send_ret_submit(client_fd, &cmd, 0, resp_buf, tx_len);
                            has_resp = false;
                        } else {
                            send_ret_submit(client_fd, &cmd, 0, NULL, 0);
                        }
                    }
                } else if (cmd.ep == 3) {
                    // EP3 IN: SWO Trace Endpoint
                    send_ret_submit(client_fd, &cmd, 0, NULL, 0);
                } else {
                    send_ret_submit(client_fd, &cmd, -1, NULL, 0);
                }
            } else if (cmd_code == 0x00000002) { // UNLINK
                USBIP_CMD_UNLINK unl;
                unl.command = cmd_code;
                if (!recv_all(client_fd, ((uint32_t *)&unl) + 1, sizeof(USBIP_CMD_UNLINK) - 4)) break;
                unl.seqnum = ntohl(unl.seqnum);
                USBIP_RET_UNLINK ret_unl;
                memset(&ret_unl, 0, sizeof(ret_unl));
                ret_unl.command = htonl(0x0004);
                ret_unl.seqnum  = htonl(unl.seqnum);
                ret_unl.status  = htonl(-ECONNRESET);
                send_all(client_fd, &ret_unl, sizeof(ret_unl));
            } else {
                break;
            }
        }
    }

    close(client_fd);
    stlink_swd_close();
    printf("[STLINK] Client disconnected.\n");
}

int main(void) {
    setlinebuf(stdout);
    setlinebuf(stderr);
    signal(SIGPIPE, SIG_IGN);

    printf("====================================================\n");
    printf(" Plink ST-LINK/V2 Server (Target Pod #2 -> Header J4)\n");
    printf(" Port: %d | VID: 0483 | PID: 3748\n", TCP_SERV_PORT);
    printf("====================================================\n");

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return 1;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port        = htons(TCP_SERV_PORT);

    if (bind(server_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen");
        return 1;
    }

    printf("[STLINK] Server listening on port %d...\n", TCP_SERV_PORT);

    while (1) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli_addr.sin_addr, ip_str, sizeof(ip_str));
        printf("[STLINK] Connection accepted from %s:%d\n", ip_str, ntohs(cli_addr.sin_port));

        handle_client(client_fd);
    }

    close(server_fd);
    return 0;
}
