/* Real DAP handlers, explicitly compiled without MMIO setup/ARM bit-bang delay. */
#define PLINK_OFFLINE_TEST
#include "../t9_programmer/usbip_dap/DAP.c"
#undef NDEBUG
#include <assert.h>

volatile uint32_t *gpio_mem, *swd_hw_mem, *swo_hw_mem;
bool swd_hw_available, swo_hw_available;

static int command(const uint8_t *request, int size, uint8_t *reply)
{
    return dap_process_request((uint8_t *)request, size, reply, 512);
}

int main(void)
{
    dap_init();
    uint8_t reply[512];
    uint8_t info[] = {0, 0xFE};
    assert(command(info, 2, reply) == 3 && reply[2] == 8);
    info[1] = 0xFD;
    assert(command(info, 2, reply) == 6 && reply[1] == 4 && reply[2] == 0);
    uint8_t status[] = {0x1B};
    assert(command(status, 1, reply) == 6 && reply[1] == 0);
    uint8_t bad_write[] = {5, 0, 2, 1, 0, 0, 0, 0, 1};
    assert(command(bad_write, sizeof(bad_write), reply) == 2 && reply[1] == 0xFF);
    uint8_t zero_baud[] = {0x19, 0, 0, 0, 0};
    assert(command(zero_baud, 5, reply) == 5);
    for (int i = 1; i < 5; ++i) assert(reply[i] == 0);
    uint8_t unsupported_transport[] = {0x17, 2};
    assert(command(unsupported_transport, 2, reply) == 2 && reply[1] == 0xFF);
    uint8_t capture[] = {0x1C, 0xFF, 0xFF};
    for (int capacity = 0; capacity < 16; ++capacity) {
        uint8_t guarded[32];
        memset(guarded, 0xA5, sizeof(guarded));
        int bytes = dap_process_request(capture, 3, guarded + 1, capacity);
        assert(bytes <= capacity);
        assert(guarded[0] == 0xA5 && guarded[capacity + 1] == 0xA5);
    }
    assert(dap_process_request(NULL, 0, reply, sizeof(reply)) == 2);
    puts("PASS: real DAP field widths, packet count, malformed writes, zero baud, transport and bounds");
    return 0;
}
