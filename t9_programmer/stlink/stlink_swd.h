#ifndef _STLINK_SWD_H_
#define _STLINK_SWD_H_

#include <stdint.h>
#include <stdbool.h>

int  stlink_swd_init(void);
void stlink_swd_close(void);
void stlink_swd_reset_target(int ms);

int  stlink_swd_line_reset(void);
int  stlink_swd_read_dpidr(uint32_t *dpidr);
int  stlink_swd_read_mem32(uint32_t addr, uint16_t count, uint32_t *data_out);
int  stlink_swd_write_mem32(uint32_t addr, uint16_t count, const uint32_t *data_in);
int  stlink_swd_read_core_reg(uint8_t reg_idx, uint32_t *val_out);
int  stlink_swd_write_core_reg(uint8_t reg_idx, uint32_t val);
int  stlink_swd_step_core(void);
int  stlink_swd_run_core(void);
int  stlink_swd_halt_core(void);
int  stlink_swd_get_status(uint8_t *status_out);

#endif
