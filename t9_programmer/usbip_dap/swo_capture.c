#define _POSIX_C_SOURCE 200809L
#include "swo_capture.h"
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

static struct {
    pthread_mutex_t lock;
    pthread_t thread;
    volatile uint32_t *hw;
    uint8_t bytes[SWO_CAPTURE_CAPACITY];
    size_t head, count;
    int revision;
    bool running, active, quit;
    swo_capture_stats stats;
} capture = {.lock = PTHREAD_MUTEX_INITIALIZER};

/* Called only with capture.lock held: one and only one FIFO consumer. */
static void drain(void)
{
    uint32_t available = capture.hw[0x14 / 4];
    if (available > capture.stats.hardware_high_water)
        capture.stats.hardware_high_water = available;
    if (capture.hw[0x10 / 4] & 2) capture.stats.status |= 0x80;
    if (capture.revision >= 2) {
        capture.stats.hardware_dropped = capture.hw[0x2C / 4];
        capture.stats.framing_errors = capture.hw[0x30 / 4];
        if (capture.stats.framing_errors) capture.stats.status |= 0x40;
    }
    /* Bounded work even if a faulty peripheral reports an impossible count. */
    if (available > 32768) available = 32768;
    while (available) {
        unsigned valid = 1;
        uint32_t word;
        if (capture.revision >= 2 && available >= 4) {
            word = capture.hw[0x20 / 4]; /* Four-byte atomic pop; count >= 4. */
            valid = 4;
        } else {
            word = capture.hw[0x08 / 4];
            if (!(word & 0x100)) break;
        }
        for (unsigned i = 0; i < valid; ++i) {
            ++capture.stats.received;
            if (capture.count == SWO_CAPTURE_CAPACITY) {
                ++capture.stats.software_dropped;
                capture.stats.status |= 0x80;
            } else {
                capture.bytes[(capture.head + capture.count) % SWO_CAPTURE_CAPACITY] = word;
                ++capture.count;
            }
            word >>= 8;
        }
        available -= valid;
    }
    if (capture.count > capture.stats.high_water)
        capture.stats.high_water = capture.count;
}

static void *worker(void *unused)
{
    (void)unused;
    const struct timespec delay = {.tv_nsec = 500000};
    for (;;) {
        pthread_mutex_lock(&capture.lock);
        bool quit = capture.quit;
        if (!quit && capture.active) drain();
        pthread_mutex_unlock(&capture.lock);
        if (quit) return NULL;
        nanosleep(&delay, NULL);
    }
}

int swo_capture_init(volatile uint32_t *registers, int revision)
{
    if (capture.running) return 0;
    if (!registers) return -1;
    capture.hw = registers;
    capture.revision = revision;
    capture.quit = false;
    capture.active = false;
    capture.head = capture.count = 0;
    memset(&capture.stats, 0, sizeof(capture.stats));
    if (pthread_create(&capture.thread, NULL, worker, NULL) != 0) return -1;
    capture.running = true;
    return 0;
}

void swo_capture_start(void)
{
    if (!capture.running) return;
    pthread_mutex_lock(&capture.lock);
    if (!capture.active) {
        capture.head = capture.count = 0;
        memset(&capture.stats, 0, sizeof(capture.stats));
        capture.hw[0] = 3; /* flush + enable, one AXI command */
        capture.active = true;
    }
    pthread_mutex_unlock(&capture.lock);
}

void swo_capture_stop(void)
{
    if (!capture.running) return;
    pthread_mutex_lock(&capture.lock);
    capture.hw[0] = 0;
    drain(); /* Retain the final hardware bytes for reads after STOP. */
    capture.active = false;
    pthread_mutex_unlock(&capture.lock);
}

void swo_capture_shutdown(void)
{
    if (!capture.running) return;
    swo_capture_stop();
    pthread_mutex_lock(&capture.lock);
    capture.quit = true;
    pthread_mutex_unlock(&capture.lock);
    pthread_join(capture.thread, NULL);
    capture.running = false;
}

size_t swo_capture_read(uint8_t *data, size_t size)
{
    if (!capture.running) return 0;
    pthread_mutex_lock(&capture.lock);
    if (size > capture.count) size = capture.count;
    for (size_t i = 0; i < size; ++i)
        data[i] = capture.bytes[(capture.head + i) % SWO_CAPTURE_CAPACITY];
    capture.head = (capture.head + size) % SWO_CAPTURE_CAPACITY;
    capture.count -= size;
    pthread_mutex_unlock(&capture.lock);
    return size;
}

void swo_capture_snapshot(swo_capture_stats *stats)
{
    pthread_mutex_lock(&capture.lock);
    *stats = capture.stats;
    stats->buffered = capture.count;
    stats->status |= capture.active ? 1 : 0;
    pthread_mutex_unlock(&capture.lock);
}
