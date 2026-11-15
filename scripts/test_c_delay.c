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

    // Line reset
    swdio_out();
    for (int i = 0; i < 60; i++) sw_write_bit(1);
    uint16_t seq = 0xE79E;
    for (int i = 0; i < 16; i++) {
        sw_write_bit(seq & 1);
        seq >>= 1;
    }
    for (int i = 0; i < 60; i++) sw_write_bit(1);
    for (int i = 0; i < 8; i++) sw_write_bit(0);

    // DPIDR request: 0xA5 (Start=1, APnDP=0, RnW=1, A=0,0, Parity=1, Stop=0, Park=1)
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
    for (int i = 0; i < 8; i++) sw_write_bit(0);

    printf("Delay %5d -> ACK=%d, DPIDR=0x%08X (Parity %s, %s)\n",
           d, ack, val,
           (parity == p_bit) ? "OK" : "ERR",
           (val == 0x2BA01477) ? "MATCH!" : "MISMATCH");
}

int main(void) {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    gpio_mem = (volatile uint32_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, GPIO_BASE);

    // Ensure reset is released
    gpio_mem[DIRM_2 / 4] &= ~(1 << PIN_NRST);
    gpio_mem[OEN_2 / 4]  &= ~(1 << PIN_NRST);
    gpio_mem[DIRM_2 / 4] |= (1 << PIN_SWCLK) | (1 << PIN_SWDIO);
    gpio_mem[OEN_2 / 4]  |= (1 << PIN_SWCLK) | (1 << PIN_SWDIO);
    set_pin(PIN_SWCLK, 1);
    set_pin(PIN_SWDIO, 1);
    usleep(10000);

    DELAY_COUNT = 40000;
    int ok_count = 0;
    for (int i = 0; i < 20; i++) {
        // Line reset
        swdio_out();
        for (int j = 0; j < 60; j++) sw_write_bit(1);
        uint16_t seq = 0xE79E;
        for (int j = 0; j < 16; j++) {
            sw_write_bit(seq & 1);
            seq >>= 1;
        }
        for (int j = 0; j < 60; j++) sw_write_bit(1);
        for (int j = 0; j < 8; j++) sw_write_bit(0);

        // DPIDR request: 0xA5
        uint8_t req = 0xA5;
        for (int j = 0; j < 8; j++) {
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
        for (int j = 0; j < 32; j++) {
            int b = sw_read_bit();
            val |= ((uint32_t)b << j);
            parity ^= b;
        }
        int p_bit = sw_read_bit();
        sw_trn();
        swdio_out();
        for (int j = 0; j < 8; j++) sw_write_bit(0);

        if (ack == 1 && val == 0x2BA01477 && parity == p_bit) {
            ok_count++;
        } else {
            printf("Fail #%d: ACK=%d DPIDR=0x%08X (Parity %s)\n",
                   i, ack, val, (parity == p_bit) ? "OK" : "ERR");
        }
        usleep(1000);
    }
    printf("Result: %d / 20 succeeded\n", ok_count);
    return 0;
}
