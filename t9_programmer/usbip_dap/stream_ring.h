#ifndef PLINK_STREAM_RING_H
#define PLINK_STREAM_RING_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define STREAM_RING_CAPACITY 65536u
typedef struct {
    uint8_t data[STREAM_RING_CAPACITY];
    size_t head, count, high_water;
    uint64_t bytes_in, bytes_out;
} stream_ring;

static inline size_t ring_space(const stream_ring *r)
{
    return STREAM_RING_CAPACITY - r->count;
}

static inline int ring_write(stream_ring *r, const void *data, size_t size)
{
    if (size > ring_space(r)) return 0; /* atomic enqueue, never partial success */
    size_t tail = (r->head + r->count) % STREAM_RING_CAPACITY;
    size_t first = STREAM_RING_CAPACITY - tail;
    if (first > size) first = size;
    memcpy(r->data + tail, data, first);
    memcpy(r->data, (const uint8_t *)data + first, size - first);
    r->count += size;
    r->bytes_in += size;
    if (r->count > r->high_water) r->high_water = r->count;
    return 1;
}

static inline size_t ring_peek(const stream_ring *r, const uint8_t **data)
{
    *data = r->data + r->head;
    size_t size = STREAM_RING_CAPACITY - r->head;
    return size < r->count ? size : r->count;
}

static inline void ring_consume(stream_ring *r, size_t size)
{
    if (size > r->count) size = r->count;
    r->head = (r->head + size) % STREAM_RING_CAPACITY;
    r->count -= size;
    r->bytes_out += size;
}

static inline size_t ring_read(stream_ring *r, void *data, size_t size)
{
    if (size > r->count) size = r->count;
    const uint8_t *part;
    size_t first = ring_peek(r, &part);
    if (first > size) first = size;
    memcpy(data, part, first);
    memcpy((uint8_t *)data + first, r->data, size - first);
    ring_consume(r, size);
    return size;
}
#endif
