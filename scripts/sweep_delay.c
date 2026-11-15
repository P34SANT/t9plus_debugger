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

#define PIN_SWCLK 0
#define PIN_SWDIO 1
#define PIN_NRST  2

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

static int DELAY_COUNT = 500;

static inline void swd_delay(void) {
    for (volatile int i = 0; i < DELAY_COUNT; i++);
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

static inline void sw_trn(void) {
    set_pin(PIN_SWCLK, 0);
    swd_delay();
    set_pin(PIN_SWCLK, 1);
    swd_delay();
}

void test_with_delay(int d) {
    DELAY_COUNT = d;

    // Line reset: 60 ones, 0xE79E, 60 ones, 8 zeros
    swdio_out();
    for (int i = 0; i < 60; i++) sw_write_bit(1);
    uint16_t seq = 0xE79E;
    for (int i = 0; i < 16; i++) {
        sw_write_bit(seq & 1);
        seq >>= 1;
    }
    for (int i = 0; i < 60; i++) sw_write_bit(1);
    for (int i = 0; i < 8; i++) sw_write_bit(0);

    // DPIDR request: 0xA5
    uint8_t req = 0xA5;
    for (int i = 0; i < 8; i++) {
        sw_write_bit(req & 1);
        req >>= 1;
    }

    swdio_in();
    sw_trn();

    int ack = 0;
    ack |= (sw_read_bit() << 0);
    ack |= (sw_read_bit() << 1);
    ack |= (sw_read_bit() << 2);

    uint32_t val = 0;
    int parity = 0;
    for (int i = 0; i < 32; i++) {
        int b = sw_read_bit();
        val |= ((uint32_t)b << i);
        parity ^= b;
    }
    int p_bit = sw_read_bit();
    sw_trn();
    swdio_out();
    set_pin(PIN_SWDIO, 1);
    for (int i = 0; i < 8; i++) sw_write_bit(0);

    printf("Delay %6d -> ACK=%d, DPIDR=0x%08X (Parity %s, %s)\n",
           d, ack, val,
           (parity == p_bit) ? "OK" : "ERR",
           (val == 0x2BA01477) ? "MATCH!" : "MISMATCH");
}

int main(void) {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    gpio_mem = (volatile uint32_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, GPIO_BASE);

    // Release HW SWD MUX to EMIO
    volatile uint32_t *hw_mem = (volatile uint32_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x43C00000);
    if (hw_mem != MAP_FAILED) {
        hw_mem[0] = 0; // Clear bit 15
    }

    // Ensure reset is released
    gpio_mem[DIRM_2 / 4] &= ~(1 << PIN_NRST);
    gpio_mem[OEN_2 / 4]  &= ~(1 << PIN_NRST);
    gpio_mem[DIRM_2 / 4] |= (1 << PIN_SWCLK) | (1 << PIN_SWDIO);
    gpio_mem[OEN_2 / 4]  |= (1 << PIN_SWCLK) | (1 << PIN_SWDIO);
    set_pin(PIN_SWCLK, 1);
    set_pin(PIN_SWDIO, 1);
    usleep(10000);

    int test_delays[] = {0, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 40000, 80000};
    for (int i = 0; i < sizeof(test_delays)/sizeof(test_delays[0]); i++) {
        test_with_delay(test_delays[i]);
        usleep(5000);
    }
    return 0;
}
