#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#define GPIO_BASE 0xE000A000
#define MASK_DATA_2_LSW 0x0010
#define DATA_2          0x0048
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
    return (gpio_mem[DATA_2 / 4] >> pin) & 1;
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
    for (volatile int i = 0; i < 50; i++);
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

void swd_reset(void);
int swd_op(int ap, int rnw, int a2, int a3, uint32_t *data);

void swd_reset(void) {
    swdio_out();
    for (int i = 0; i < 60; i++) sw_write_bit(1);
    uint16_t seq = 0xE79E;
    for (int i = 0; i < 16; i++) {
        sw_write_bit(seq & 1);
        seq >>= 1;
    }
    for (int i = 0; i < 60; i++) sw_write_bit(1);
    for (int i = 0; i < 8; i++) sw_write_bit(0);
}

void sw_reset_and_reinit(void) {
    swd_reset();
    uint32_t dpidr = 0, ctrl = 0x50000000, sel = 0, csw = 0xA2000012;
    swd_op(0, 1, 0, 0, &dpidr);
    swd_op(0, 0, 1, 0, &ctrl);
    swd_op(0, 0, 0, 1, &sel);
    swd_op(1, 0, 0, 0, &csw);
}

int swd_op(int ap, int rnw, int a2, int a3, uint32_t *data) {
    int p = (ap ^ rnw ^ a2 ^ a3) & 1;
    uint8_t req = 0x81 | (p << 5) | (a3 << 4) | (a2 << 3) | (rnw << 2) | (ap << 1);

    swdio_out();
    for (int i = 0; i < 8; i++) {
        sw_write_bit(req & 1);
        req >>= 1;
    }

    swdio_in();
    sw_clock_cycle(); // Turnaround

    int b0 = sw_read_bit();
    int b1 = sw_read_bit();
    int b2 = sw_read_bit();
    int ack = (b0 << 0) | (b1 << 1) | (b2 << 2);

    if (ack == 1) { // OK
        if (rnw) {
            uint32_t val = 0;
            int par = 0;
            for (int i = 0; i < 32; i++) {
                int b = sw_read_bit();
                val |= ((uint32_t)b << i);
                par ^= b;
            }
            int p_bit = sw_read_bit();
            if (par != p_bit) ack = -1;
            if (data) *data = val;

            sw_clock_cycle(); // Turnaround
            swdio_out();
        } else {
            sw_clock_cycle(); // Turnaround
            swdio_out();

            uint32_t val = *data;
            int par = 0;
            for (int i = 0; i < 32; i++) {
                int b = (val >> i) & 1;
                sw_write_bit(b);
                par ^= b;
            }
            sw_write_bit(par);
        }
        set_pin(PIN_SWDIO, 0);
        for (int i = 0; i < 8; i++) sw_clock_cycle();
    } else {
        sw_clock_cycle();
        swdio_out();
        set_pin(PIN_SWDIO, 0);
        for (int i = 0; i < 8; i++) sw_clock_cycle();
    }
    set_pin(PIN_SWDIO, 1);
    return ack;
}

int main(void) {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    gpio_mem = (volatile uint32_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, GPIO_BASE);

    gpio_mem[DIRM_2 / 4] |= (1 << PIN_NRST) | (1 << PIN_SWCLK) | (1 << PIN_SWDIO);
    gpio_mem[OEN_2 / 4]  |= (1 << PIN_NRST) | (1 << PIN_SWCLK) | (1 << PIN_SWDIO);
    set_pin(PIN_NRST, 1);
    set_pin(PIN_SWCLK, 1);
    set_pin(PIN_SWDIO, 1);
    usleep(10000);

    swd_reset();

    uint32_t dpidr = 0, ctrl = 0x50000000, sel = 0, csw = 0xA2000012;
    swd_op(0, 1, 0, 0, &dpidr);
    swd_op(0, 0, 1, 0, &ctrl);
    swd_op(0, 0, 0, 1, &sel);
    swd_op(1, 0, 0, 0, &csw);

    printf("Starting 5000 Write TAR -> Read DRW tests...\n");
    int wait_count = 0, ok_count = 0, err_count = 0;
    for (int t = 0; t < 5000; t++) {
        uint32_t addr = 0xE000ED00 + (t % 4) * 4;
        int tar_ack = swd_op(1, 0, 1, 0, &addr); // Write TAR
        if (tar_ack != 1) {
            printf("  [Test %2d] Write TAR FAILED: ack=%d\n", t, tar_ack);
            swd_reset();
            continue;
        }

        // Read DRW
        int ap = 1, rnw = 1, a2 = 1, a3 = 1;
        int p = (ap ^ rnw ^ a2 ^ a3) & 1;
        uint8_t req = 0x81 | (p << 5) | (a3 << 4) | (a2 << 3) | (rnw << 2) | (ap << 1);

        swdio_out();
        for (int i = 0; i < 8; i++) {
            sw_write_bit(req & 1);
            req >>= 1;
        }
        swdio_in();
        // Turnaround cycle: sample pin in the middle of low phase and high phase
        set_pin(PIN_SWCLK, 0);
        swd_delay();
        int trn_low = read_pin(PIN_SWDIO);
        set_pin(PIN_SWCLK, 1);
        swd_delay();
        int trn_high = read_pin(PIN_SWDIO);

        int b0 = sw_read_bit();
        int b1 = sw_read_bit();
        int b2 = sw_read_bit();
        int ack = (b0 << 0) | (b1 << 1) | (b2 << 2);

        if (ack == 1) {
            ok_count++;
            // consume data
            for (int i = 0; i < 33; i++) sw_read_bit();
            sw_clock_cycle();
            swdio_out();
            set_pin(PIN_SWDIO, 0);
            sw_clock_cycle();
            set_pin(PIN_SWDIO, 1);
        } else if (ack == 2) {
            wait_count++;
            // turnaround after WAIT
            sw_clock_cycle();
            swdio_out();
            set_pin(PIN_SWDIO, 0);
            for (int i = 0; i < 8; i++) sw_clock_cycle();
            set_pin(PIN_SWDIO, 1);
        } else {
            err_count++;
            printf("  [Test %2d] BAD ACK: %d (bits: %d %d %d) trn_low=%d trn_high=%d\n", t, ack, b0, b1, b2, trn_low, trn_high);
            printf("  Next 10 bits: ");
            for (int i = 0; i < 10; i++) {
                printf("%d ", sw_read_bit());
            }
            printf("\n");
            // protocol error recovery
            sw_reset_and_reinit();
        }
    }
    printf("Results: OK=%d, WAIT=%d, ERR=%d\n", ok_count, wait_count, err_count);
    return 0;
}
