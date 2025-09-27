#ifndef _MSD_HANDLER_H_
#define _MSD_HANDLER_H_

#include "config.h"

void msd_init(void);
void msd_task(void);
bool msd_ready(void);

// Callbacks for TinyUSB
bool tud_msd_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize);
int32_t tud_msd_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize);
int32_t tud_msd_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize);

#endif