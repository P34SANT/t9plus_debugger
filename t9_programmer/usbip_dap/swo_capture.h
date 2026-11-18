#ifndef PLINK_SWO_CAPTURE_H
#define PLINK_SWO_CAPTURE_H
#include <stddef.h>
#include <stdint.h>

#define SWO_CAPTURE_CAPACITY (1024u * 1024u)
typedef struct {
    uint32_t buffered, high_water, hardware_high_water, framing_errors;
    uint64_t received, software_dropped;
    uint32_t hardware_dropped;
    uint8_t status;
} swo_capture_stats;

int swo_capture_init(volatile uint32_t *registers, int revision);
void swo_capture_start(void);
void swo_capture_stop(void);
void swo_capture_shutdown(void);
size_t swo_capture_read(uint8_t *data, size_t size);
void swo_capture_snapshot(swo_capture_stats *stats);
#endif
