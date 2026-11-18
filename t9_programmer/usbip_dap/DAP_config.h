#ifndef _DAP_CONFIG_H_
#define _DAP_CONFIG_H_

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <time.h>
#include <string.h>

#define DAP_CONFIG_DEFAULT_PORT        1 // DAP_PORT_SWD
#define DAP_CONFIG_DEFAULT_CLOCK       1000000 // 1 MHz

#define DAP_CONFIG_PACKET_SIZE         512
#define DAP_CONFIG_PACKET_COUNT        8

#define DAP_CONFIG_VENDOR_STR          "Plink"
#define DAP_CONFIG_PRODUCT_STR         "Plink"
#define DAP_CONFIG_SER_NUM_STR         "0204000100000002"
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
extern volatile uint32_t *swd_hw_mem;
extern volatile uint32_t *swo_hw_mem;
extern bool swd_hw_available;
extern bool swo_hw_available;

#define SWD_HW_BASE         0x43C00000
#define SWD_REG_CTRL        0x00
#define SWD_REG_CLK_DIV     0x04
#define SWD_REG_WDATA       0x08
#define SWD_REG_RDATA       0x0C
#define SWD_REG_STATUS      0x10
#define SWD_REG_RAW_LEN     0x14
#define SWD_REG_ID          0x18

// SWO Trace Engine (0x43C20000) - axi_swo_engine.v
#define SWO_HW_BASE         0x43C20000
#define SWO_REG_CTRL        0x00  // [0]=enable, [1]=flush, [2]=channel(Pod), [3]=invert
#define SWO_REG_CLK_DIV     0x04  // Baud prescaler: 100MHz / div (div=50 -> 2.0 Mbps)
#define SWO_REG_DATA_BYTE   0x08  // Pop byte: [7:0]=data, [8]=valid
#define SWO_REG_DATA_PEEK   0x0C  // Peek byte without popping
#define SWO_REG_STATUS      0x10  // [0]=enabled, [1]=overflow, [2]=empty, [3]=full
#define SWO_REG_FIFO_COUNT  0x14  // Bytes in FIFO
#define SWO_REG_ID          0x18  // 0x53574F01 ("SWO1")
#define SWO_REG_TOTAL       0x1C  // Total received byte counter

#define SWO_CORE_ID         0x53574F02
#define SWO_FIFO_SIZE       (1024u * 1024u) /* Continuously drained DDR ring */
#define SWO_FPGA_CLOCK      100000000

extern volatile uint32_t *swo_hw_mem;
extern bool swo_hw_available;

static inline bool is_fpga_operating(void) {
    FILE *ready = fopen("/run/plink/fpga-ready", "r");
    char abi[32] = {0};
    if (!ready) return false;
    bool verified = fgets(abi, sizeof(abi), ready) && !strcmp(abi, "plink-abi-2\n");
    fclose(ready);
    if (!verified) return false;
    FILE *f = fopen("/sys/class/fpga_manager/fpga0/state", "r");
    if (!f) return false;
    char state[32] = {0};
    if (!fgets(state, sizeof(state), f)) {
        fclose(f);
        return false;
    }
    fclose(f);
    return (strncmp(state, "operating", 9) == 0);
}

static inline int zynq_swd_hw_init(void) {
    if (swd_hw_mem != NULL) return 0;

    // Fail-safe protection: NEVER touch /dev/mem at PL addresses if FPGA is not operating!
    if (!is_fpga_operating()) {
        printf("[WARN] FPGA PL is not operating. Hardware acceleration disabled safely.\n");
        swd_hw_available = false;
        swd_hw_mem = NULL;
        return 0;
    }

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) return -1;
    swd_hw_mem = (volatile uint32_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, SWD_HW_BASE);
    close(fd);
    if (swd_hw_mem == MAP_FAILED) {
        swd_hw_mem = NULL;
        swd_hw_available = false;
        return -1;
    }

    if (swd_hw_mem[SWD_REG_ID / 4] == 0x53574401 ||
        swd_hw_mem[SWD_REG_ID / 4] == 0x53574402) {
        swd_hw_available = true;
        // Default: 2.0 MHz (div=24), HW_EN=1 (bit 15), IDLE_CYCLES=8 (bits 12:8)
        swd_hw_mem[SWD_REG_CLK_DIV / 4] = 24;
        swd_hw_mem[SWD_REG_CTRL / 4] = (1 << 17) | (1 << 15) | (8 << 8);
    } else {
        swd_hw_available = false;
        swd_hw_mem[SWD_REG_CTRL / 4] = 0; // Clear HW_EN bit 15 to grant EMIO pin control
    }
    return 0;
}

static inline int zynq_swo_hw_init(void) {
    if (swo_hw_mem != NULL) return 0;

    // Fail-safe: never touch /dev/mem at PL addresses if FPGA is not operating
    if (!is_fpga_operating()) {
        swo_hw_available = false;
        swo_hw_mem = NULL;
        return 0;
    }

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) return -1;
    swo_hw_mem = (volatile uint32_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, SWO_HW_BASE);
    close(fd);
    if (swo_hw_mem == MAP_FAILED) {
        swo_hw_mem = NULL;
        swo_hw_available = false;
        return -1;
    }

    if ((swo_hw_mem[SWO_REG_ID / 4] & 0xFFFFFF00) == 0x53574F00 &&
        (swo_hw_mem[SWO_REG_ID / 4] & 0xFF) >= 1 &&
        (swo_hw_mem[SWO_REG_ID / 4] & 0xFF) <= 2) {
        swo_hw_available = true;
        // Default: disabled, Pod #1 (J3 Pin 6 / T10), 2.0 Mbps (div=50)
        swo_hw_mem[SWO_REG_CTRL / 4]    = 0x00000000;
        swo_hw_mem[SWO_REG_CLK_DIV / 4] = SWO_FPGA_CLOCK / 2000000;
    } else {
        swo_hw_available = false;
    }
    return 0;
}

// BOOT0 / SWO shared line (J3 Pin 6 = T10 = EMIO 57):
// Keep this line Hi-Z even after capture stops: the target can still drive SWO.
// BOOT0 control requires explicit user action and different target wiring.
#define BOOT0_SYSFS_GPIO  569
static inline int zynq_swo_release_pin(void)
{
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", BOOT0_SYSFS_GPIO);
    FILE *f = fopen(path, "w");
    if (!f) {
        perror("SWO pin direction");
        return -1;
    }
    int result = fputs("in", f);
    int closed = fclose(f);
    return (result == EOF || closed != 0) ? -1 : 0;
}

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
    gpio_mem[DIRM_2 / 4] &= ~((1 << PIN_SWCLK) | (1 << PIN_SWDIO));
    gpio_mem[OEN_2 / 4]  &= ~((1 << PIN_SWCLK) | (1 << PIN_SWDIO));
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
    return (gpio_mem[DATA_2_RO / 4] >> pin) & 1;
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
#ifdef PLINK_OFFLINE_TEST
    return;
#else
    zynq_gpio_init();
    zynq_swd_hw_init();
    zynq_swo_hw_init();
    zynq_pin_dir_in(PIN_SWCLK);
    zynq_pin_dir_in(PIN_SWDIO);
    zynq_set_pin(PIN_SWCLK, 1);
    zynq_set_pin(PIN_SWDIO, 1);
    DAP_CONFIG_nRESET_write(1);
#endif
}

extern struct timespec dap_last_prog_ts;

static inline void zynq_d3_led_raw(int red_on, int green_on) {
    if (!gpio_mem) return;
    uint32_t mask = (~((1 << MIO_D3_RED) | (1 << MIO_D3_GREEN))) & 0xFFFF;
    uint32_t data = ((red_on ? 1 : 0) << MIO_D3_RED) | ((green_on ? 1 : 0) << MIO_D3_GREEN);
    gpio_mem[MASK_DATA_1_LSW / 4] = (mask << 16) | data;
}

static inline void zynq_d3_led_set(int green_on) {
    zynq_d3_led_raw(0, green_on);
}

static inline void dap_mark_programming(void) {
    clock_gettime(CLOCK_MONOTONIC, &dap_last_prog_ts);
}

static inline void dap_activity_idle(void) {
    zynq_d3_led_raw(0, 0);
}

static inline void dap_activity_blink(void) {
    static struct timespec last_toggle = {0, 0};
    static int phase = 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long prog_ms = (now.tv_sec - dap_last_prog_ts.tv_sec) * 1000 +
                   (now.tv_nsec - dap_last_prog_ts.tv_nsec) / 1000000;
    bool is_programming = (prog_ms < 1200); // Active flash programming in last 1.2s

    long elapsed_ms = (now.tv_sec - last_toggle.tv_sec) * 1000 + 
                      (now.tv_nsec - last_toggle.tv_nsec) / 1000000;

    int period_ms = is_programming ? 60 : 80;
    if (elapsed_ms >= period_ms) {
        phase ^= 1;
        if (is_programming) {
            // Programming: blinks green only (Red stays OFF)
            zynq_d3_led_raw(0, phase ? 1 : 0);
        } else {
            // Debugging: blinks both, alternating (never at the same time)
            if (phase) {
                zynq_d3_led_raw(0, 1); // Green ON, Red OFF
            } else {
                zynq_d3_led_raw(1, 0); // Red ON, Green OFF
            }
        }
        last_toggle = now;
    }
}

static inline void DAP_CONFIG_DISCONNECT(void) {
    if (swd_hw_available && swd_hw_mem) {
        /* Revision 2 RELEASE bit overrides FPGA pin drive; v1 drops the mux. */
        swd_hw_mem[SWD_REG_CTRL / 4] = (1u << 17) | (1u << 15);
    }
    zynq_pin_dir_in(PIN_SWCLK);
    zynq_pin_dir_in(PIN_SWDIO);
    dap_activity_idle();
}

static inline void DAP_CONFIG_CONNECT_SWD(void) {
    if (swd_hw_available && swd_hw_mem)
        swd_hw_mem[SWD_REG_CTRL / 4] = (1u << 15) | (8u << 8);
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
            dap_activity_idle();
        }
    } else if (index == 1) { // Running
        if (state) {
            dap_activity_blink();
        } else {
            dap_activity_idle();
        }
    }
}

static inline void DAP_CONFIG_DELAY(uint32_t cycles) {
#ifdef PLINK_OFFLINE_TEST
    (void)cycles;
#else
    if (cycles < 10) cycles = 10;
    __asm__ volatile (
        "1: subs %0, %0, #1 \n\t"
        "   bne 1b          \n\t"
        : "+r" (cycles)
        :
        : "cc"
    );
#endif
}

#endif // _DAP_CONFIG_H_
