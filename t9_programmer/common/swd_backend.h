#ifndef PLINK_SWD_BACKEND_H
#define PLINK_SWD_BACKEND_H
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>

static inline uint64_t swd_monotonic_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + now.tv_nsec;
}

static inline int swd_wait(volatile uint32_t *hw, uint32_t previous, uint32_t *status)
{
    uint64_t deadline = swd_monotonic_ns() + 250000000u;
    unsigned spins = 0;
    int v2 = hw[0x18 / 4] == 0x53574402;
    do {
        *status = hw[0x10 / 4];
        if (!(*status & 1) && (!v2 || hw[0x1C / 4] != previous)) return 0;
        if ((++spins & 63) == 0 && swd_monotonic_ns() >= deadline) return -1;
    } while (1);
}

/* Request layout matches CMSIS-DAP APnDP/RnW/A2/A3 bits. No automatic write replay. */
static inline int swd_hw_transfer(volatile uint32_t *hw, unsigned request,
                                  uint32_t *data, unsigned idle, unsigned turnaround,
                                  int data_phase)
{
    if (idle > 31 || turnaround < 1 || turnaround > 4) return 8;
    if (!(request & 2) && !data) return 8;
    uint32_t previous = hw[0x1C / 4];
    if (!(request & 2)) hw[0x08 / 4] = *data;
    uint32_t control = (1u << 15) | (idle << 8) |
        (((request >> 2) & 3) << 3) | ((request & 2) << 1) |
        ((request & 1) << 1) | ((turnaround - 1) << 18) | (!!data_phase << 20) | 1;
    atomic_thread_fence(memory_order_seq_cst);
    hw[0] = control;
    uint32_t status;
    if (swd_wait(hw, previous, &status)) return 8;
    int ack = (status >> 1) & 7;
    if (ack != 1 && ack != 2 && ack != 4) return 8;
    if (ack == 1 && (request & 2)) {
        if (status & 16) return 8;
        if (data) *data = hw[0x0C / 4];
    }
    return ack;
}

static inline int swd_hw_raw(volatile uint32_t *hw, uint32_t *data,
                             unsigned bits, int input)
{
    if (!data || bits < 1 || bits > 32) return -1;
    uint32_t previous = hw[0x1C / 4];
    hw[0x08 / 4] = *data;
    hw[0x14 / 4] = bits;
    atomic_thread_fence(memory_order_seq_cst);
    hw[0] = (1u << 15) | (1u << 14) | (!!input << 16) | 1;
    uint32_t status;
    if (swd_wait(hw, previous, &status)) return -1;
    if (input) *data = hw[0x0C / 4];
    return 0;
}
#endif
