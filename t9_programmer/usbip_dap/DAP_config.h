#ifndef _DAP_CONFIG_H_
#define _DAP_CONFIG_H_

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <time.h>

#define DAP_CONFIG_DEFAULT_PORT        1 // DAP_PORT_SWD
#define DAP_CONFIG_DEFAULT_CLOCK       1000000 // 1 MHz

#define DAP_CONFIG_PACKET_SIZE         512
#define DAP_CONFIG_PACKET_COUNT        1

#define DAP_CONFIG_VENDOR_STR          "ARM"
#define DAP_CONFIG_PRODUCT_STR         "CMSIS-DAP"
#define DAP_CONFIG_SER_NUM_STR         "0204000100000000"
#define DAP_CONFIG_CMSIS_DAP_VER_STR   "2.0.0"

#define DAP_CONFIG_RESET_TARGET_FN     zynq_reset_target
#define DAP_CONFIG_PERFORMANCE_ATTR

#define DAP_CONFIG_DELAY_CONSTANT      500000
#define DAP_CONFIG_FAST_CLOCK          10000000

// Zynq-7000 GPIO Registers
#define GPIO_BASE           0xE000A000
#define MASK_DATA_1_LSW     0x0008  // Bank 1 LSW (MIO 32..47)
#define MASK_DATA_2_LSW     0x0010  // Bank 2 LSW (pins 54..69)
#define DATA_1              0x0044  // Bank 1 Output Data
#define DATA_2              0x0048  // Bank 2 Input Data (Workaround for Zynq EMIO DATA_RO bug)
#define DATA_2_RO           0x0068  // Bank 2 Input Data RO
#define DIRM_1              0x0244  // Bank 1 Direction
#define OEN_1               0x0248  // Bank 1 Output Enable
#define DIRM_2              0x0284  // Bank 2 Direction (1=out, 0=in)
#define OEN_2               0x0288  // Bank 2 Output Enable (1=enable)

#define PIN_SWCLK           0       // GPIO 54 (EMIO 0)
#define PIN_SWDIO           1       // GPIO 55 (EMIO 1)
#define PIN_NRST            2       // GPIO 56 (EMIO 2)

#define MIO_D3_RED          5       // MIO 37 = Bank 1, bit 5
#define MIO_D3_GREEN        6       // MIO 38 = Bank 1, bit 6

extern volatile uint32_t *gpio_mem;

static inline int zynq_gpio_init(void) {
    if (gpio_mem != NULL) return 0;
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        return -1;
    }
    gpio_mem = (volatile uint32_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, GPIO_BASE);
    close(fd);
    if (gpio_mem == MAP_FAILED) {
        perror("mmap");
        gpio_mem = NULL;
        return -1;
    }

    // Configure SWCLK, SWDIO as outputs; nRST initially as input (pullup keeps high)
    gpio_mem[DIRM_2 / 4] |= (1 << PIN_SWCLK) | (1 << PIN_SWDIO);
    gpio_mem[OEN_2 / 4]  |= (1 << PIN_SWCLK) | (1 << PIN_SWDIO);
    gpio_mem[DIRM_2 / 4] &= ~(1 << PIN_NRST);
    gpio_mem[OEN_2 / 4]  &= ~(1 << PIN_NRST);

    // Initial state: SWCLK high, SWDIO high
    uint32_t mask = (~((1 << PIN_SWCLK) | (1 << PIN_SWDIO))) & 0xFFFF;
    uint32_t data = (1 << PIN_SWCLK) | (1 << PIN_SWDIO);
    gpio_mem[MASK_DATA_2_LSW / 4] = (mask << 16) | data;

    // Configure D3 Red & Green (MIO 37/38) as outputs, initially solid OFF (Red=0, Green=0)
    gpio_mem[DIRM_1 / 4] |= (1 << MIO_D3_RED) | (1 << MIO_D3_GREEN);
    gpio_mem[OEN_1 / 4]  |= (1 << MIO_D3_RED) | (1 << MIO_D3_GREEN);
    uint32_t mask_d3 = (~((1 << MIO_D3_RED) | (1 << MIO_D3_GREEN))) & 0xFFFF;
    gpio_mem[MASK_DATA_1_LSW / 4] = (mask_d3 << 16) | 0;

    return 0;
}

static inline void zynq_set_pin(int pin, int val) {
    uint32_t mask = (~(1 << pin)) & 0xFFFF;
    uint32_t data = (val ? 1 : 0) << pin;
    gpio_mem[MASK_DATA_2_LSW / 4] = (mask << 16) | data;
}

static inline int zynq_read_pin(int pin) {
    return (gpio_mem[DATA_2 / 4] >> pin) & 1;
}

static inline void zynq_pin_dir_out(int pin) {
    gpio_mem[DIRM_2 / 4] |= (1 << pin);
    gpio_mem[OEN_2 / 4]  |= (1 << pin);
}

static inline void zynq_pin_dir_in(int pin) {
    gpio_mem[DIRM_2 / 4] &= ~(1 << pin);
    gpio_mem[OEN_2 / 4]  &= ~(1 << pin);
}

// SWCLK
static inline void DAP_CONFIG_SWCLK_TCK_write(int value) {
    zynq_set_pin(PIN_SWCLK, value);
}

static inline void DAP_CONFIG_SWCLK_TCK_set(void) {
    zynq_set_pin(PIN_SWCLK, 1);
}

static inline void DAP_CONFIG_SWCLK_TCK_clr(void) {
    zynq_set_pin(PIN_SWCLK, 0);
}

static inline int DAP_CONFIG_SWCLK_TCK_read(void) {
    return zynq_read_pin(PIN_SWCLK);
}

// SWDIO
static inline void DAP_CONFIG_SWDIO_TMS_write(int value) {
    zynq_set_pin(PIN_SWDIO, value);
}

static inline void DAP_CONFIG_SWDIO_TMS_set(void) {
    zynq_set_pin(PIN_SWDIO, 1);
}

static inline void DAP_CONFIG_SWDIO_TMS_clr(void) {
    zynq_set_pin(PIN_SWDIO, 0);
}

static inline int DAP_CONFIG_SWDIO_TMS_read(void) {
    return zynq_read_pin(PIN_SWDIO);
}

static inline void DAP_CONFIG_SWDIO_TMS_in(void) {
    zynq_pin_dir_in(PIN_SWDIO);
}

static inline void DAP_CONFIG_SWDIO_TMS_out(void) {
    zynq_pin_dir_out(PIN_SWDIO);
}

// nRESET (Open-drain)
static inline void DAP_CONFIG_nRESET_write(int value) {
    if (value) {
        // High = Release reset (float input, external pullup pulls high)
        zynq_pin_dir_in(PIN_NRST);
        zynq_set_pin(PIN_NRST, 1);
    } else {
        // Low = Assert reset (drive low)
        zynq_set_pin(PIN_NRST, 0);
        zynq_pin_dir_out(PIN_NRST);
    }
}

static inline int DAP_CONFIG_nRESET_read(void) {
    return zynq_read_pin(PIN_NRST);
}

static inline void zynq_reset_target(void) {
    DAP_CONFIG_nRESET_write(0);
    usleep(25000); // 25ms assert
    DAP_CONFIG_nRESET_write(1);
    usleep(25000); // 25ms recover
}

// Unused JTAG stubs
static inline void DAP_CONFIG_TDI_write(int value) { (void)value; }
static inline void DAP_CONFIG_TDO_write(int value) { (void)value; }
static inline void DAP_CONFIG_nTRST_write(int value) { (void)value; }
static inline int DAP_CONFIG_TDO_read(void) { return 0; }
static inline int DAP_CONFIG_TDI_read(void) { return 0; }
static inline int DAP_CONFIG_nTRST_read(void) { return 0; }

// Port Control
static inline void DAP_CONFIG_SETUP(void) {
    zynq_gpio_init();
    zynq_pin_dir_out(PIN_SWCLK);
    zynq_pin_dir_out(PIN_SWDIO);
    zynq_set_pin(PIN_SWCLK, 1);
    zynq_set_pin(PIN_SWDIO, 1);
    DAP_CONFIG_nRESET_write(1);
}

static inline void zynq_d3_led_set(int green_on) {
    if (!gpio_mem) return;
    // Red (bit 5) is always kept OFF (0), Green (bit 6) set according to green_on
    uint32_t mask = (~((1 << MIO_D3_RED) | (1 << MIO_D3_GREEN))) & 0xFFFF;
    uint32_t data = (green_on ? (1 << MIO_D3_GREEN) : 0);
    gpio_mem[MASK_DATA_1_LSW / 4] = (mask << 16) | data;
}

static inline void dap_activity_blink(void) {
    static struct timespec last_toggle = {0, 0};
    static int led_state = 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long elapsed_ms = (now.tv_sec - last_toggle.tv_sec) * 1000 + 
                      (now.tv_nsec - last_toggle.tv_nsec) / 1000000;
    if (elapsed_ms >= 60) {
        led_state ^= 1;
        zynq_d3_led_set(led_state);
        last_toggle = now;
    }
}

static inline void DAP_CONFIG_DISCONNECT(void) {
    zynq_pin_dir_in(PIN_SWCLK);
    zynq_pin_dir_in(PIN_SWDIO);
    zynq_d3_led_set(0);
}

static inline void DAP_CONFIG_CONNECT_SWD(void) {
    zynq_pin_dir_out(PIN_SWCLK);
    zynq_pin_dir_out(PIN_SWDIO);
    zynq_set_pin(PIN_SWCLK, 1);
    zynq_set_pin(PIN_SWDIO, 1);
    DAP_CONFIG_nRESET_write(1);
}

static inline void DAP_CONFIG_CONNECT_JTAG(void) {
    DAP_CONFIG_CONNECT_SWD();
}

static inline void DAP_CONFIG_LED(int index, int state) {
    if (index == 0) { // Connect
        if (state == 0) {
            zynq_d3_led_set(0);
        }
    } else if (index == 1) { // Running
        if (state) {
            dap_activity_blink();
        } else {
            zynq_d3_led_set(0);
        }
    }
}

static inline void DAP_CONFIG_DELAY(uint32_t cycles) {
    if (cycles < 10) cycles = 10;
    __asm__ volatile (
        "1: subs %0, %0, #1 \n\t"
        "   bne 1b          \n\t"
        : "+r" (cycles)
        :
        : "cc"
    );
}

#endif // _DAP_CONFIG_H_
