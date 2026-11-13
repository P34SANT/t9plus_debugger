#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#define GPIO_BASE 0xE000A000
#define MASK_DATA_2_LSW 0x0010
#define DATA_2_RO       0x0068
#define DIRM_2          0x0284
#define OEN_2           0x0288

#define PIN_SWCLK 0  // GPIO 54 (EMIO 0)
#define PIN_SWDIO 1  // GPIO 55 (EMIO 1)
#define PIN_NRST  2  // GPIO 56 (EMIO 2)
#define PIN_BOOT0 3  // GPIO 57 (EMIO 3)

static volatile uint32_t *gpio_mem = NULL;

static inline void set_pin(int pin, int val) {
    uint32_t mask = (~(1 << pin)) & 0xFFFF;
    uint32_t data = (val ? 1 : 0) << pin;
    gpio_mem[MASK_DATA_2_LSW / 4] = (mask << 16) | data;
}

static inline int read_pin(int pin) {
    return (gpio_mem[DATA_2_RO / 4] >> pin) & 1;
}

static inline void swdio_out(void) {
    gpio_mem[DIRM_2 / 4] |= (1 << PIN_SWDIO);
    gpio_mem[OEN_2 / 4]  |= (1 << PIN_SWDIO);
}

static inline void swdio_in(void) {
    gpio_mem[DIRM_2 / 4] &= ~(1 << PIN_SWDIO);
    gpio_mem[OEN_2 / 4]  &= ~(1 << PIN_SWDIO);
}

static inline void swd_delay(void) {
    for (volatile int i = 0; i < 20; i++);
}

static inline void sw_clock_cycle(void) {
    set_pin(PIN_SWCLK, 0);
    swd_delay();
    set_pin(PIN_SWCLK, 1);
    swd_delay();
}

static inline void sw_write_bit(int bit) {
    set_pin(PIN_SWDIO, bit);
    set_pin(PIN_SWCLK, 0);
    swd_delay();
    set_pin(PIN_SWCLK, 1);
    swd_delay();
}

static inline int sw_read_bit(void) {
    set_pin(PIN_SWCLK, 0);
    swd_delay();
    int b = read_pin(PIN_SWDIO);
    set_pin(PIN_SWCLK, 1);
    swd_delay();
    return b;
}

void swd_reset_and_switch(void) {
    swdio_out();
    // 50+ ones
    for (int i = 0; i < 60; i++) sw_write_bit(1);
    // JTAG to SWD sequence: 0xE79E (0b0111100111100111 LSB first)
    uint16_t seq = 0xE79E;
    for (int i = 0; i < 16; i++) {
        sw_write_bit(seq & 1);
        seq >>= 1;
    }
    // 50+ ones
    for (int i = 0; i < 60; i++) sw_write_bit(1);
    // 8 idle zeroes
    for (int i = 0; i < 8; i++) sw_write_bit(0);
}

int main(void) {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }
    gpio_mem = (volatile uint32_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, GPIO_BASE);
    if (gpio_mem == MAP_FAILED) { perror("mmap"); return 1; }

    // Init pins
    gpio_mem[DIRM_2 / 4] |= (1 << PIN_NRST) | (1 << PIN_SWCLK) | (1 << PIN_SWDIO);
    gpio_mem[OEN_2 / 4]  |= (1 << PIN_NRST) | (1 << PIN_SWCLK) | (1 << PIN_SWDIO);
    set_pin(PIN_NRST, 1);
    set_pin(PIN_SWCLK, 1);
    set_pin(PIN_SWDIO, 1);
    usleep(10000);

    printf("Sending SWD reset and JTAG-to-SWD sequence...\n");
    swd_reset_and_switch();

    // Read DPIDR: request = 0xA5 (Start=1, APnDP=0, RnW=1, A2=0, A3=0, Parity=1, Stop=0, Park=1)
    uint8_t req = 0xA5;
    for (int i = 0; i < 8; i++) {
        sw_write_bit(req & 1);
        req >>= 1;
    }

    // Turnaround
    swdio_in();
    sw_clock_cycle();

    // ACK (3 bits)
    int ack = 0;
    ack |= (sw_read_bit() << 0);
    ack |= (sw_read_bit() << 1);
    ack |= (sw_read_bit() << 2);

    printf("SWD ACK: %d\n", ack);

    if (ack == 1) {
        uint32_t val = 0;
        int parity = 0;
        for (int i = 0; i < 32; i++) {
            int b = sw_read_bit();
            val |= ((uint32_t)b << i);
            parity ^= b;
        }
        int p_bit = sw_read_bit();
        printf("DPIDR: 0x%08X (Parity %s)\n", val, (parity == p_bit) ? "VALID" : "INVALID");
        if (val == 0x2BA01477) {
            printf("SUCCESS! Read STM32F407 CoreSight DPIDR = 0x2BA01477!\n");
        }
    } else {
        printf("ACK failed (expected 1, got %d)\n", ack);
    }

    // Turnaround & idle
    sw_clock_cycle();
    swdio_out();
    for (int i = 0; i < 8; i++) sw_write_bit(0);

    return 0;
}
