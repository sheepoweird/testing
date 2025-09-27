#include "msd_handler.h"
#include "tusb.h"

#define BLOCK_SIZE 512
#define BLOCK_COUNT 4096

static uint8_t ram_disk[BLOCK_SIZE * BLOCK_COUNT] __attribute__((aligned(BLOCK_SIZE)));

void msd_init() {
    memset(ram_disk, 0, sizeof(ram_disk));
    printf("MSD Initialized with %d blocks\n", BLOCK_COUNT);
}

void msd_task() {
    // MSD periodic tasks can go here
}

// TinyUSB MSD callbacks
bool tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize) {
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    if (lba >= BLOCK_COUNT) return -1;
    memcpy(buffer, &ram_disk[lba * BLOCK_SIZE + offset], bufsize);
    return bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    if (lba >= BLOCK_COUNT) return -1;
    memcpy(&ram_disk[lba * BLOCK_SIZE + offset], buffer, bufsize);
    return bufsize;
}