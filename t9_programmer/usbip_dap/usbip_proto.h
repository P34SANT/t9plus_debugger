#ifndef _USBIP_PROTO_H_
#define _USBIP_PROTO_H_

#include <stdint.h>

typedef uint8_t   byte;
typedef uint16_t  word;

// Standard USB Descriptor Types
#define USB_DESCRIPTOR_DEVICE           0x01
#define USB_DESCRIPTOR_CONFIGURATION    0x02
#define USB_DESCRIPTOR_STRING           0x03
#define USB_DESCRIPTOR_INTERFACE        0x04
#define USB_DESCRIPTOR_ENDPOINT         0x05
#define USB_DESCRIPTOR_DEVICE_QUALIFIER 0x06
#define USB_DESCRIPTOR_BOS              0x0F
#define USB_DESCRIPTOR_DEVICE_CAPABILITY 0x10

typedef struct __attribute__ ((__packed__)) {
    byte bLength;
    byte bDescriptorType;
    word bcdUSB;
    byte bDeviceClass;
    byte bDeviceSubClass;
    byte bDeviceProtocol;
    byte bMaxPacketSize0;
    word idVendor;
    word idProduct;
    word bcdDevice;
    byte iManufacturer;
    byte iProduct;
    byte iSerialNumber;
    byte bNumConfigurations;
} USB_DEVICE_DESCRIPTOR;

typedef struct __attribute__ ((__packed__)) {
    byte bLength;
    byte bDescriptorType;
    word wTotalLength;
    byte bNumInterfaces;
    byte bConfigurationValue;
    byte iConfiguration;
    byte bmAttributes;
    byte bMaxPower;
} USB_CONFIGURATION_DESCRIPTOR;

typedef struct __attribute__ ((__packed__)) {
    byte bLength;
    byte bDescriptorType;
    byte bInterfaceNumber;
    byte bAlternateSetting;
    byte bNumEndpoints;
    byte bInterfaceClass;
    byte bInterfaceSubClass;
    byte bInterfaceProtocol;
    byte iInterface;
} USB_INTERFACE_DESCRIPTOR;

typedef struct __attribute__ ((__packed__)) {
    byte bLength;
    byte bDescriptorType;
    byte bEndpointAddress;
    byte bmAttributes;
    word wMaxPacketSize;
    byte bInterval;
} USB_ENDPOINT_DESCRIPTOR;

typedef struct __attribute__ ((__packed__)) {
    byte bLength;
    byte bType;
    word bcdUSB;
    byte bDeviceClass;
    byte bDeviceSubClass;
    byte bDeviceProtocol;
    byte bMaxPacketSize0;
    byte bNumConfigurations;
    byte bReserved;
} USB_DEVICE_QUALIFIER_DESCRIPTOR;

typedef struct __attribute__ ((__packed__)) {
    USB_CONFIGURATION_DESCRIPTOR dev_conf0;
    USB_INTERFACE_DESCRIPTOR     dev_int0;
    USB_ENDPOINT_DESCRIPTOR      ep_out;
    USB_ENDPOINT_DESCRIPTOR      ep_in;
} CONFIG_CMSIS_DAP_V2;

// USB/IP Protocol Structs
typedef struct __attribute__ ((__packed__)) {
    word version;
    word command;
    int32_t status;
} OP_REQ_DEVLIST;

typedef struct __attribute__ ((__packed__)) {
    word version;
    word command;
    int32_t status;
    int32_t nExportedDevice;
} OP_REP_DEVLIST_HEADER;

typedef struct __attribute__ ((__packed__)) {
    char    usbPath[256];
    char    busID[32];
    int32_t busnum;
    int32_t devnum;
    int32_t speed;
    word    idVendor;
    word    idProduct;
    word    bcdDevice;
    byte    bDeviceClass;
    byte    bDeviceSubClass;
    byte    bDeviceProtocol;
    byte    bConfigurationValue;
    byte    bNumConfigurations;
    byte    bNumInterfaces;
} OP_REP_DEVLIST_DEVICE;

typedef struct __attribute__ ((__packed__)) {
    byte bInterfaceClass;
    byte bInterfaceSubClass;
    byte bInterfaceProtocol;
    byte padding;
} OP_REP_DEVLIST_INTERFACE;

typedef struct __attribute__ ((__packed__)) {
    word    version;
    word    command;
    int32_t status;
    char    busID[32];
} OP_REQ_IMPORT;

typedef struct __attribute__ ((__packed__)) {
    word    version;
    word    command;
    int32_t status;
    char    usbPath[256];
    char    busID[32];
    int32_t busnum;
    int32_t devnum;
    int32_t speed;
    word    idVendor;
    word    idProduct;
    word    bcdDevice;
    byte    bDeviceClass;
    byte    bDeviceSubClass;
    byte    bDeviceProtocol;
    byte    bConfigurationValue;
    byte    bNumConfigurations;
    byte    bNumInterfaces;
} OP_REP_IMPORT;

typedef struct __attribute__ ((__packed__, aligned)) {
    int32_t command;
    int32_t seqnum;
    int32_t devid;
    int32_t direction;
    int32_t ep;
    int32_t transfer_flags;
    int32_t transfer_buffer_length;
    int32_t start_frame;
    int32_t number_of_packets;
    int32_t interval;
    byte    setup[8];
} USBIP_CMD_SUBMIT;

typedef struct __attribute__ ((__packed__, aligned)) {
    int32_t command;
    int32_t seqnum;
    int32_t devid;
    int32_t direction;
    int32_t ep;
    int32_t status;
    int32_t actual_length;
    int32_t start_frame;
    int32_t number_of_packets;
    int32_t error_count;
    byte    setup[8];
} USBIP_RET_SUBMIT;

typedef struct __attribute__ ((__packed__, aligned)) {
    int32_t command;
    int32_t seqnum;
    int32_t devid;
    int32_t direction;
    int32_t ep;
    int32_t seqnum_urb;
    byte    padding[24];
} USBIP_CMD_UNLINK;

typedef struct __attribute__ ((__packed__, aligned)) {
    int32_t command;
    int32_t seqnum;
    int32_t devid;
    int32_t direction;
    int32_t ep;
    int32_t status;
    byte    padding[24];
} USBIP_RET_UNLINK;

typedef struct __attribute__ ((__packed__)) {
    byte bmRequestType;
    byte bRequest;
    union {
        struct { byte lowByte; byte hiByte; };
        uint16_t W;
    } wValue;
    union {
        struct { byte lowByte; byte hiByte; };
        uint16_t W;
    } wIndex;
    word wLength;
} setup_pkt_t;

#endif // _USBIP_PROTO_H_
