/* Offline deterministic SWO software-ring test, no worker or real MMIO. */
#include "../t9_programmer/usbip_dap/swo_capture.c"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

int main(void)
{
    uint32_t registers[16] = {0};
    capture.hw = registers;
    capture.revision = 2;
    capture.running = true; /* Enables read API, no worker thread is created. */
    capture.head = SWO_CAPTURE_CAPACITY - 2;
    registers[0x14 / 4] = 4;
    registers[0x20 / 4] = 0x04030201;
    drain();
    uint8_t bytes[8] = {0};
    assert(swo_capture_read(bytes, sizeof(bytes)) == 4);
    assert(bytes[0] == 1 && bytes[1] == 2 && bytes[2] == 3 && bytes[3] == 4);
    capture.count = SWO_CAPTURE_CAPACITY - 1;
    drain();
    swo_capture_stats stats;
    swo_capture_snapshot(&stats);
    assert(stats.buffered == SWO_CAPTURE_CAPACITY);
    assert(stats.high_water == SWO_CAPTURE_CAPACITY);
    assert(stats.software_dropped == 3 && (stats.status & 0x80));
    registers[0x14 / 4] = 0;
    registers[0x30 / 4] = 5;
    registers[0x2C / 4] = 7;
    drain();
    swo_capture_snapshot(&stats);
    assert(stats.framing_errors == 5 && stats.hardware_dropped == 7);
    assert(stats.status & 0x40);
    capture.running = false;
    puts("PASS: SWO software ring wrap, bounded capacity, dropped bytes, framing telemetry");
    return 0;
}
