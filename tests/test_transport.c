/* Offline only: socketpairs, fake DAP, no USB driver or target access. */
#define main plink_server_main
#include "../t9_programmer/usbip_dap/usbip_server.c"
#undef main
#include "../t9_programmer/usbip_dap/dap_validate.h"
#undef NDEBUG
#include <assert.h>

struct timespec dap_last_prog_ts;
void dap_init(void) {}
void dap_session_close(void) {}
void dap_write_stats(FILE *file) { fputs("{}", file); }
int dap_process_request(uint8_t *req, int len, uint8_t *reply, int cap)
{
    (void)cap;
    reply[0] = len ? req[0] : 0xFF;
    return 1;
}

static USBIP_RET_SUBMIT completion(int fd)
{
    USBIP_RET_SUBMIT reply;
    assert(recv_all(fd, &reply, sizeof(reply)));
    assert(ntohl(reply.command) == 3);
    assert(reply.devid == 0 && reply.direction == 0 && reply.ep == 0);
    return reply;
}

int main(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    pending_in_init();
    USBIP_CMD_SUBMIT request = {.seqnum = 12, .direction = 1, .ep = 1,
                               .transfer_buffer_length = 512};
    assert(pending_in_push(&request));
    assert(complete_unlink(sockets[0], 13, 12));
    USBIP_RET_UNLINK cancelled;
    assert(recv_all(sockets[1], &cancelled, sizeof(cancelled)));
    assert((int32_t)ntohl(cancelled.status) == -ECONNRESET);
    char byte;
    assert(recv(sockets[1], &byte, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    assert(complete_unlink(sockets[0], 14, 12));
    assert(recv_all(sockets[1], &cancelled, sizeof(cancelled)));
    assert(cancelled.status == 0);
    assert(pending_in_count == 0);

    composite_session *session = calloc(1, sizeof(*session));
    assert(session);
    assert(ring_write(&session->uart[1].rx, "abcdef", 6));
    request.ep = 5; request.transfer_buffer_length = 2;
    assert(pending_in_push(&request));
    assert(complete_pending(sockets[0], session));
    USBIP_RET_SUBMIT reply = completion(sockets[1]);
    assert(ntohl(reply.actual_length) == 2);
    char bytes[8] = {0};
    assert(recv_all(sockets[1], bytes, 2) && !memcmp(bytes, "ab", 2));
    assert(session->uart[1].rx.count == 4); /* No truncated/discarded serial bytes. */
    assert(ring_read(&session->uart[1].rx, bytes, 8) == 4 && !memcmp(bytes, "cdef", 4));

    for (unsigned i = 0; i < DAP_RESPONSE_DEPTH; ++i) {
        session->replies[i].data[0] = i;
        session->replies[i].size = 1;
        ++session->reply_count;
        request.ep = 1; request.seqnum = 20 + i; request.transfer_buffer_length = 512;
        assert(pending_in_push(&request));
    }
    assert(complete_pending(sockets[0], session));
    for (unsigned i = 0; i < DAP_RESPONSE_DEPTH; ++i) {
        reply = completion(sockets[1]);
        assert(ntohl(reply.seqnum) == 20 + i);
        assert(recv_all(sockets[1], &byte, 1) && (unsigned char)byte == i);
    }
    assert(!session->reply_count);
    request.ep = 2;
    for (unsigned i = 0; i < MAX_PENDING_IN; ++i) {
        request.seqnum = 100 + i;
        assert(pending_in_push(&request));
    }
    assert(!pending_in_push(&request));
    assert(pending_in_count == MAX_PENDING_IN);
    pending_in_init();

    uint8_t truncated_write[] = {6, 0, 2, 0, 1, 0, 0, 0, 0};
    assert(!dap_request_valid(truncated_write, sizeof(truncated_write)));
    uint8_t valid_write[] = {6, 0, 1, 0, 1, 0, 0, 0, 0};
    assert(dap_request_valid(valid_write, sizeof(valid_write)));
    uint8_t truncated_stlink[] = {0xF2, 0x33, 0, 0, 0, 0, 255, 255};
    uint8_t response[512];
    assert(!process_stlink_command(truncated_stlink, sizeof(truncated_stlink),
                                   response, sizeof(response)));
    uint8_t random[512];
    uint32_t rng = 1;
    for (unsigned iteration = 0; iteration < 100000; ++iteration) {
        for (unsigned i = 0; i < sizeof(random); ++i) {
            rng = rng * 1664525u + 1013904223u;
            random[i] = rng >> 24;
        }
        (void)dap_request_valid(random, iteration % 513);
    }
    free(session);
    close(sockets[0]); close(sockets[1]);
    puts("PASS: unlink lifecycle, CDC retention, reply pipelining, bounded queues, parser bounds");
    return 0;
}
