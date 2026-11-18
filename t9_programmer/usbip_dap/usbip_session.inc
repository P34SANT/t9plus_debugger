/* Included by usbip_server.c: one bounded, single-owner composite session. */
#define MAX_URB_BYTES STREAM_RING_CAPACITY
#define DAP_RESPONSE_DEPTH DAP_CONFIG_PACKET_COUNT

typedef struct {
    int fd;
    bool connecting;
    uint64_t retry_at, reconnects, errors;
    stream_ring rx, tx;
} uart_link;

typedef struct {
    uart_link uart[2];
    struct { uint8_t data[DAP_MAX_PACKET]; size_t size; } replies[DAP_RESPONSE_DEPTH];
    size_t reply_head, reply_count;
    uint8_t payload[MAX_URB_BYTES];
    bool notify[2];
    uint64_t last_snapshot;
} composite_session;

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void uart_link_close(uart_link *u)
{
    if (u->fd >= 0) close(u->fd);
    u->fd = -1;
    u->connecting = false;
    u->retry_at = monotonic_ms() + 1000;
}

static void uart_link_connect(uart_link *u, int port)
{
    if (u->fd >= 0 || monotonic_ms() < u->retry_at) return;
    u->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (u->fd < 0) { u->retry_at = monotonic_ms() + 1000; return; }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int result = connect(u->fd, (struct sockaddr *)&addr, sizeof(addr));
    if (result == 0) { ++u->reconnects; return; }
    if (errno == EINPROGRESS) { u->connecting = true; return; }
    ++u->errors;
    uart_link_close(u);
}

static void uart_link_pump(uart_link *u, short events)
{
    if (u->fd < 0) return;
    if (u->connecting && (events & (POLLOUT | POLLERR | POLLHUP))) {
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(u->fd, SOL_SOCKET, SO_ERROR, &error, &size) || error) {
            ++u->errors;
            uart_link_close(u);
            return;
        }
        u->connecting = false;
        ++u->reconnects;
    }
    if (u->connecting) return;
    if (events & POLLIN) {
        uint8_t data[4096];
        for (int budget = 0; budget < 16 && ring_space(&u->rx); ++budget) {
            size_t size = ring_space(&u->rx);
            if (size > sizeof(data)) size = sizeof(data);
            ssize_t n = recv(u->fd, data, size, MSG_DONTWAIT);
            if (n > 0) { ring_write(&u->rx, data, (size_t)n); continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            ++u->errors;
            uart_link_close(u);
            return;
        }
    }
    if (events & POLLOUT) {
        const uint8_t *data;
        size_t size = ring_peek(&u->tx, &data);
        if (size) {
            ssize_t n = send(u->fd, data, size, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n > 0) ring_consume(&u->tx, (size_t)n);
            else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                ++u->errors;
                uart_link_close(u);
            }
        }
    }
    if (events & (POLLERR | POLLHUP | POLLNVAL)) {
        ++u->errors;
        uart_link_close(u);
    }
}

static bool complete_pending(int fd, composite_session *s)
{
    USBIP_CMD_SUBMIT cmd;
    while (s->reply_count && pending_in_pop_ep(1, &cmd)) {
        size_t index = s->reply_head;
        if ((size_t)cmd.transfer_buffer_length < s->replies[index].size) {
            if (!send_ret_submit(fd, &cmd, -EMSGSIZE, NULL, 0)) return false;
            continue; /* Preserve the atomic response for an adequately sized IN. */
        }
        if (!send_ret_submit(fd, &cmd, 0, s->replies[index].data,
                             s->replies[index].size)) return false;
        s->reply_head = (index + 1) % DAP_RESPONSE_DEPTH;
        --s->reply_count;
    }
    for (int port = 0; port < 2; ++port) {
        while (s->uart[port].rx.count && pending_in_pop_ep(port ? 5 : 3, &cmd)) {
            uint8_t data[512];
            size_t size = (size_t)cmd.transfer_buffer_length;
            if (size > sizeof(data)) size = sizeof(data);
            size = ring_read(&s->uart[port].rx, data, size);
            if (!send_ret_submit(fd, &cmd, 0, data, size)) return false;
        }
        if (s->notify[port] && pending_in_pop_ep(port ? 4 : 2, &cmd)) {
            uint8_t notification[10] = {0xA1, 0x20, 0, 0, port ? 3 : 1, 0, 2, 0, 3, 0};
            if (!send_ret_submit(fd, &cmd, 0, notification, sizeof(notification))) return false;
            s->notify[port] = false;
        }
    }
    return true;
}

static bool receive_submit(int fd, USBIP_CMD_SUBMIT *cmd)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->command = 1;
    if (!recv_all(fd, (uint8_t *)cmd + 4, sizeof(*cmd) - 4)) return false;
    cmd->seqnum = ntohl(cmd->seqnum);
    cmd->devid = ntohl(cmd->devid);
    cmd->direction = ntohl(cmd->direction);
    cmd->ep = ntohl(cmd->ep);
    cmd->transfer_flags = ntohl(cmd->transfer_flags);
    cmd->transfer_buffer_length = ntohl(cmd->transfer_buffer_length);
    cmd->number_of_packets = ntohl(cmd->number_of_packets);
    /* Negative lengths and unsupported isochronous payloads cannot be framed safely. */
    return cmd->direction >= 0 && cmd->direction <= 1 &&
           cmd->transfer_buffer_length >= 0 &&
           (uint32_t)cmd->transfer_buffer_length <= MAX_URB_BYTES &&
           (cmd->number_of_packets == 0 || cmd->number_of_packets == -1);
}

static bool complete_unlink(int fd, uint32_t sequence, uint32_t target)
{
    USBIP_RET_UNLINK response = {0};
    response.command = htonl(4);
    response.seqnum = htonl(sequence);
    bool removed = pending_in_unlink(target, NULL);
    response.status = htonl(removed ? -ECONNRESET : 0);
    return send_all(fd, &response, sizeof(response));
}

static void run_plink_session(int fd)
{
    if (!is_fpga_operating()) return;
    composite_session *s = calloc(1, sizeof(*s));
    if (!s) return;
    s->uart[0].fd = s->uart[1].fd = -1;
    s->notify[0] = s->notify[1] = true;
    pending_in_init();
    dap_init(); /* Only after the pod lock has been acquired. */
    while (!stop_requested) {
        if (monotonic_ms() - s->last_snapshot >= 1000) {
            FILE *stats = fopen("/run/plink/pod1.json.tmp", "w");
            if (stats) {
                dap_write_stats(stats);
                if (fclose(stats) == 0)
                    rename("/run/plink/pod1.json.tmp", "/run/plink/pod1.json");
            }
            s->last_snapshot = monotonic_ms();
        }
        for (int port = 0; port < 2; ++port)
            uart_link_connect(&s->uart[port], 2000 + port);
        if (!complete_pending(fd, s)) break;
        struct pollfd polls[3] = {{ .fd = fd, .events = POLLIN }};
        for (int port = 0; port < 2; ++port) {
            uart_link *u = &s->uart[port];
            polls[port + 1].fd = u->fd;
            if (ring_space(&u->rx)) polls[port + 1].events |= POLLIN;
            if (u->connecting || u->tx.count) polls[port + 1].events |= POLLOUT;
        }
        int result = poll(polls, 3, 20);
        if (result < 0) { if (errno == EINTR) continue; break; }
        for (int port = 0; port < 2; ++port)
            uart_link_pump(&s->uart[port], polls[port + 1].revents);
        if (!complete_pending(fd, s)) break;
        if (!(polls[0].revents & POLLIN)) {
            if (polls[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
            dap_activity_idle();
            continue;
        }
        uint32_t code;
        if (!recv_all(fd, &code, sizeof(code))) break;
        code = ntohl(code);
        if (code == 2) {
            USBIP_CMD_UNLINK unlink_cmd = {0};
            if (!recv_all(fd, (uint8_t *)&unlink_cmd + 4, sizeof(unlink_cmd) - 4)) break;
            /* A cancelled SUBMIT must NEVER receive a second completion. */
            if (!complete_unlink(fd, ntohl(unlink_cmd.seqnum),
                                 ntohl(unlink_cmd.seqnum_urb))) break;
            continue;
        }
        if (code != 1) break;
        USBIP_CMD_SUBMIT cmd;
        if (!receive_submit(fd, &cmd)) break;
        size_t length = (size_t)cmd.transfer_buffer_length;
        if (cmd.direction == 0 && !recv_all(fd, s->payload, length)) break;
        if (cmd.ep < 0 || cmd.ep > 5 ||
            ((cmd.ep == 2 || cmd.ep == 4) && cmd.direction != 1)) {
            if (!send_ret_submit(fd, &cmd, -EPIPE, NULL, 0)) break;
        } else if (cmd.ep == 0) {
            if (!handle_control_transfer(fd, &cmd, s->payload, length)) break;
        } else if (!length) {
            if (!send_ret_submit(fd, &cmd, 0, NULL, 0)) break;
        } else if (cmd.direction == 1) {
            if (!pending_in_push(&cmd) &&
                !send_ret_submit(fd, &cmd, -ENOSPC, NULL, 0)) break;
        } else if (cmd.ep == 1) {
            if (length > DAP_MAX_PACKET || s->reply_count == DAP_RESPONSE_DEPTH) {
                if (!send_ret_submit(fd, &cmd, -ENOSPC, NULL, 0)) break;
                continue;
            }
            size_t index = (s->reply_head + s->reply_count) % DAP_RESPONSE_DEPTH;
            int size = dap_process_request(s->payload, length, s->replies[index].data, DAP_MAX_PACKET);
            if (size < 0 || size > DAP_MAX_PACKET) break;
            s->replies[index].size = size;
            ++s->reply_count;
            if (!send_ret_submit(fd, &cmd, 0, NULL, length)) break;
        } else {
            int port = cmd.ep == 5;
            bool queued = ring_write(&s->uart[port].tx, s->payload, length);
            if (!send_ret_submit(fd, &cmd, queued ? 0 : -ENOSPC,
                                 NULL, queued ? length : 0)) break;
        }
    }
    dap_session_close();
    FILE *stats = fopen("/run/plink/pod1.json.tmp", "w");
    if (stats) {
        dap_write_stats(stats);
        if (fclose(stats) == 0) rename("/run/plink/pod1.json.tmp", "/run/plink/pod1.json");
    }
    for (int port = 0; port < 2; ++port) {
        uart_link *u = &s->uart[port];
        fprintf(stderr, "[CDC%d] rx=%llu tx=%llu rx_high=%zu tx_high=%zu reconnects=%llu errors=%llu unsent=%zu\n",
                port, (unsigned long long)u->rx.bytes_in, (unsigned long long)u->tx.bytes_out,
                u->rx.high_water, u->tx.high_water, (unsigned long long)u->reconnects,
                (unsigned long long)u->errors, u->tx.count);
        uart_link_close(u);
    }
    free(s);
}
