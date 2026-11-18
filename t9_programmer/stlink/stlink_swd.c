/*
 * stlink_swd.c - High-Speed ADIv5 SWD Engine for Pod #2 (Header J4)
 * Directly drives Zynq-7000 EMIO Bank 2 (Pins 9, 10, 11) via /dev/mem
 * Pinout (Header J4):
 *   - Pin 3: SWCLK1 (EMIO 9)
 *   - Pin 4: SWDIO1 (EMIO 10)
 *   - Pin 5: nRST1  (EMIO 11, open-drain)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include "stlink_swd.h"
#include "../common/swd_backend.h"

#define GPIO_BASE           0xE000A000
#define MASK_DATA_2_LSW     0x0010
#define DATA_2_RO           0x0068
#define DIRM_2              0x0284
#define OEN_2               0x0288

#define PIN_CLK1            9
#define PIN_DIO1            10
#define PIN_RST1            11

// CoreSight ADIv5 Register Offsets
#define DP_REG_IDCODE       0x00
#define DP_REG_ABORT        0x00
#define DP_REG_CTRL_STAT    0x04
#define DP_REG_SELECT       0x08
#define DP_REG_RDBUFF       0x0C

#define AP_REG_CSW          0x00
#define AP_REG_TAR          0x04
#define AP_REG_DRW          0x0C

// Cortex-M Debug Registers
#define DHCSR               0xE000EDF0
#define DCRSR               0xE000EDF4
#define DCRDR               0xE000EDF8
#define DEMCR               0xE000EDFC

static volatile uint32_t *gpio = NULL;
static volatile uint32_t *hardware = NULL;

static inline void set_pin(int pin, int val) {
    uint32_t mask = (~(1 << pin)) & 0xFFFF;
    uint32_t data = (val ? 1 : 0) << pin;
    gpio[MASK_DATA_2_LSW / 4] = (mask << 16) | data;
}

static inline int read_pin(int pin) {
    return (gpio[DATA_2_RO / 4] >> pin) & 1;
}

static inline void dir_out(int pin) {
    gpio[DIRM_2 / 4] |= (1 << pin);
    gpio[OEN_2 / 4]  |= (1 << pin);
}

static inline void dir_in(int pin) {
    gpio[DIRM_2 / 4] &= ~(1 << pin);
    gpio[OEN_2 / 4]  &= ~(1 << pin);
}

static inline void clk_high(void) { set_pin(PIN_CLK1, 1); }
static inline void clk_low(void)  { set_pin(PIN_CLK1, 0); }
static inline void dio_high(void) { set_pin(PIN_DIO1, 1); }
static inline void dio_low(void)  { set_pin(PIN_DIO1, 0); }
static inline int  dio_read(void) { return read_pin(PIN_DIO1); }

static inline void swd_delay(void) {
    // Fast tight loop for SWD clock cycle (~5-10 MHz)
    for (volatile int i = 0; i < 4; i++);
}

static inline void swd_clock_pulse(void) {
    clk_low();
    swd_delay();
    clk_high();
    swd_delay();
}

static inline void swd_write_bit(int bit) {
    set_pin(PIN_DIO1, bit);
    clk_low();
    swd_delay();
    clk_high();
    swd_delay();
}

static inline int swd_read_bit(void) {
    clk_low();
    swd_delay();
    int b = dio_read();
    clk_high();
    swd_delay();
    return b;
}

static inline uint32_t parity(uint32_t val) {
    val ^= val >> 16;
    val ^= val >> 8;
    val ^= val >> 4;
    val &= 0x0F;
    return (0x6996 >> val) & 1;
}

int stlink_swd_init(void) {
    if (gpio != NULL) return 0;
    FILE *marker = fopen("/run/plink/fpga-ready", "r");
    char abi[32] = {0};
    if (!marker) return -1;
    int verified = fgets(abi, sizeof(abi), marker) && !strcmp(abi, "plink-abi-2\n");
    fclose(marker);
    if (!verified) return -1;
    FILE *state = fopen("/sys/class/fpga_manager/fpga0/state", "r");
    char value[32] = {0};
    if (!state) return -1;
    int ready = fgets(value, sizeof(value), state) && !strncmp(value, "operating", 9);
    fclose(state);
    if (!ready) return -1;
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) return -1;
    hardware = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x43C10000);
    if (hardware == MAP_FAILED) { hardware = NULL; close(fd); return -1; }
    if (hardware[0x18 / 4] != 0x53574401 && hardware[0x18 / 4] != 0x53574402) {
        munmap((void *)hardware, 4096); hardware = NULL; close(fd); return -1;
    }
    hardware[0x04 / 4] = 24; /* Same 2 MHz default as Pod 1. */
    gpio = (volatile uint32_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, GPIO_BASE);
    close(fd);
    if (gpio == MAP_FAILED) {
        gpio = NULL;
        munmap((void *)hardware, 4096);
        hardware = NULL;
        return -1;
    }

    hardware[0] = (1u << 17) | (1u << 15);
    dir_in(PIN_CLK1);
    dir_in(PIN_DIO1);
    dir_in(PIN_RST1); // Reset released (external pullup)
    clk_high();
    dio_high();

    if (stlink_swd_line_reset()) {
        stlink_swd_close();
        return -1;
    }
    return 0;
}

void stlink_swd_close(void) {
    if (!gpio) return;
    if (hardware) hardware[0] = (1u << 17) | (1u << 15);
    dir_in(PIN_CLK1);
    dir_in(PIN_DIO1);
    dir_in(PIN_RST1);
    munmap((void *)gpio, 4096);
    munmap((void *)hardware, 4096);
    gpio = hardware = NULL;
}

void stlink_swd_reset_target(int ms) {
    if (!gpio) return;
    set_pin(PIN_RST1, 0);
    dir_out(PIN_RST1);
    usleep(ms * 1000);
    dir_in(PIN_RST1);
    usleep(25000);
}

int stlink_swd_line_reset(void) {
    if (!gpio) return -1;
    if (hardware) {
        uint32_t sequences[] = {0xFFFFFFFF, 0xFFFFFFFF, 0xE79E,
                                0xFFFFFFFF, 0xFFFFFFFF, 0};
        unsigned lengths[] = {32, 32, 16, 32, 32, 8};
        for (unsigned i = 0; i < 6; ++i)
            if (swd_hw_raw(hardware, &sequences[i], lengths[i], 0)) return -1;
        return 0;
    }
    dir_out(PIN_DIO1);
    dio_high();

    // >50 clocks with DIO=1
    for (int i = 0; i < 64; i++) swd_clock_pulse();

    // JTAG-to-SWD switch sequence: 0xE79E (16 bits, LSB first: 0x9E, 0xE7)
    uint16_t seq = 0xE79E;
    for (int i = 0; i < 16; i++) {
        swd_write_bit(seq & 1);
        seq >>= 1;
    }

    // >50 clocks with DIO=1
    dio_high();
    for (int i = 0; i < 64; i++) swd_clock_pulse();

    // >=8 idle cycles with DIO=0
    dio_low();
    for (int i = 0; i < 16; i++) swd_clock_pulse();
    dio_high();

    return 0;
}

static int swd_transaction(int rnw, int apndp, int addr, uint32_t *data) {
    if (hardware) {
        unsigned request = ((addr >> 2) & 3) << 2 | (rnw ? 2 : 0) | (apndp ? 1 : 0);
        int ack = 8;
        for (int retry = 0; retry <= 100; ++retry) {
            ack = swd_hw_transfer(hardware, request, data, 8, 1, 0);
            if (ack != 2) break; /* Only WAIT is safe to retry. */
        }
        return ack;
    }
    if (!gpio) return 8;
    // Packet Header: [Start=1, APnDP, RnW, A2, A3, Parity, Stop=0, Park=1]
    uint32_t req = ((addr >> 2) & 3) << 2 | (rnw ? 2 : 0) | (apndp ? 1 : 0);
    uint32_t par = parity(req);
    uint8_t hdr = 0x81 | (par << 5) | (req << 1);

    dir_out(PIN_DIO1);
    for (int i = 0; i < 8; i++) {
        swd_write_bit(hdr & 1);
        hdr >>= 1;
    }

    // Turnaround (Host releases line)
    dir_in(PIN_DIO1);
    swd_clock_pulse();

    // Sample 3-bit ACK (LSB first)
    int ack = 0;
    for (int i = 0; i < 3; i++) {
        ack |= (swd_read_bit() << i);
    }

    if (ack != 1) { // 1 = OK
        // Turnaround & recover
        swd_clock_pulse();
        dir_out(PIN_DIO1);
        dio_low();
        for (int i = 0; i < 8; i++) swd_clock_pulse();
        dio_high();
        return ack;
    }

    if (rnw) {
        // Read 32-bit data
        uint32_t val = 0;
        for (int i = 0; i < 32; i++) {
            val |= ((uint32_t)swd_read_bit() << i);
        }
        int read_par = swd_read_bit();
        if (parity(val) != (uint32_t)read_par) {
            ack = 4; // Parity fault
        }
        if (data) *data = val;

        // Turnaround
        swd_clock_pulse();
        dir_out(PIN_DIO1);
    } else {
        // Turnaround for write
        swd_clock_pulse();
        dir_out(PIN_DIO1);

        uint32_t val = data ? *data : 0;
        for (int i = 0; i < 32; i++) {
            swd_write_bit(val & 1);
            val >>= 1;
        }
        swd_write_bit(parity(data ? *data : 0));
    }

    // ADIv5 Idle cycles
    dio_low();
    for (int i = 0; i < 8; i++) swd_clock_pulse();
    dio_high();

    return ack;
}

int stlink_swd_read_dpidr(uint32_t *dpidr) {
    stlink_swd_line_reset();
    return (swd_transaction(1, 0, DP_REG_IDCODE, dpidr) == 1) ? 0 : -1;
}

int stlink_swd_read_mem32(uint32_t addr, uint16_t count, uint32_t *data_out) {
    if (!gpio || !data_out || count == 0 || (addr & 3)) return -1;

    // Select AP 0, Bank 0
    uint32_t select = 0x00000000;
    if (swd_transaction(0, 0, DP_REG_SELECT, &select) != 1) return -1;

    // Set CSW: 32-bit incrementing transfer
    uint32_t csw = 0x23000052;
    if (swd_transaction(0, 1, AP_REG_CSW, &csw) != 1) return -1;

    while (count) {
        unsigned chunk = (1024 - (addr & 1023)) / 4;
        if (chunk > count) chunk = count;
        if (swd_transaction(0, 1, AP_REG_TAR, &addr) != 1) return -1;
        uint32_t dummy;
        if (swd_transaction(1, 1, AP_REG_DRW, &dummy) != 1) return -1;
        for (unsigned i = 0; i < chunk; ++i) {
            int ap = i + 1 != chunk;
            if (swd_transaction(1, ap, ap ? AP_REG_DRW : DP_REG_RDBUFF,
                                &data_out[i]) != 1) return -1;
        }
        addr += chunk * 4; data_out += chunk; count -= chunk;
    }
    return 0;
}

int stlink_swd_write_mem32(uint32_t addr, uint16_t count, const uint32_t *data_in) {
    if (!gpio || !data_in || count == 0 || (addr & 3)) return -1;

    // Select AP 0, Bank 0
    uint32_t select = 0x00000000;
    if (swd_transaction(0, 0, DP_REG_SELECT, &select) != 1) return -1;

    // Set CSW: 32-bit incrementing transfer
    uint32_t csw = 0x23000052;
    if (swd_transaction(0, 1, AP_REG_CSW, &csw) != 1) return -1;

    // Set TAR
    if (swd_transaction(0, 1, AP_REG_TAR, &addr) != 1) return -1;

    // Write words
    for (uint16_t i = 0; i < count; i++) {
        if (i && !((addr + i * 4) & 1023)) {
            uint32_t next = addr + i * 4;
            if (swd_transaction(0, 1, AP_REG_TAR, &next) != 1) return -1;
        }
        uint32_t val = data_in[i];
        if (swd_transaction(0, 1, AP_REG_DRW, &val) != 1) return -1;
    }
    uint32_t status;
    if (swd_transaction(1, 0, DP_REG_RDBUFF, &status) != 1) return -1;
    return 0;
}

int stlink_swd_step_core(void) {
    uint32_t step_cmd = 0xA05F000B; // DBGKEY | C_DEBUGEN | C_STEP | C_MASKINTS
    return stlink_swd_write_mem32(DHCSR, 1, &step_cmd);
}

int stlink_swd_run_core(void) {
    uint32_t run_cmd = 0xA05F0001; // DBGKEY | C_DEBUGEN
    return stlink_swd_write_mem32(DHCSR, 1, &run_cmd);
}

int stlink_swd_halt_core(void) {
    uint32_t halt_cmd = 0xA05F0003; // DBGKEY | C_DEBUGEN | C_HALT
    return stlink_swd_write_mem32(DHCSR, 1, &halt_cmd);
}

int stlink_swd_get_status(uint8_t *status_out) {
    uint32_t dhcsr = 0;
    if (stlink_swd_read_mem32(DHCSR, 1, &dhcsr) < 0) {
        *status_out = 0x00;
        return -1;
    }
    // S_HALT is bit 17
    *status_out = (dhcsr & (1 << 17)) ? 0x09 : 0x00;
    return 0;
}

int stlink_swd_read_core_reg(uint8_t reg_idx, uint32_t *val_out) {
    uint32_t dcrsr = reg_idx & 0x7F;
    if (stlink_swd_write_mem32(DCRSR, 1, &dcrsr)) return -1;
    uint64_t deadline = swd_monotonic_ns() + 50000000;
    uint32_t status;
    do {
        if (stlink_swd_read_mem32(DHCSR, 1, &status)) return -1;
        if (status & (1u << 16)) break;
        if (swd_monotonic_ns() > deadline) return -1;
    } while (1);
    return stlink_swd_read_mem32(DCRDR, 1, val_out);
}

int stlink_swd_write_core_reg(uint8_t reg_idx, uint32_t val) {
    if (stlink_swd_write_mem32(DCRDR, 1, &val)) return -1;
    uint32_t dcrsr = (1 << 16) | (reg_idx & 0x7F); // Write flag
    return stlink_swd_write_mem32(DCRSR, 1, &dcrsr);
}
