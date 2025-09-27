#ifndef _MSD_HANDLER_H_
#define _MSD_HANDLER_H_

#include "pico/stdlib.h"
#include "tusb.h"

void msd_init(void);
void msd_task(void);
bool msd_is_ready(void);

// TinyUSB callbacks
bool tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize);
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize);
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize);

#endif