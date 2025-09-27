#include "msd_handler.h"
#include "tusb.h"
#include <string.h>

#define BLOCK_SIZE  512
#define BLOCK_COUNT 8192  // 4MB storage

static uint8_t ram_disk[BLOCK_SIZE * BLOCK_COUNT] __attribute__((aligned(4)));

void msd_init(void) {
    memset(ram_disk, 0, sizeof(ram_disk));
    
    // Add basic MBR signature
    ram_disk[510] = 0x55;
    ram_disk[511] = 0xAA;
    
    printf("MSD: Initialized %lu KB RAM disk\n", sizeof(ram_disk) / 1024);
}

void msd_task(void) {
    // Periodic tasks can be added here if needed
}

// TinyUSB MSC Callbacks
bool tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize) {
    (void) lun;
    
    switch (scsi_cmd[0]) {
        case 0x1E: // PREVENT ALLOW MEDIUM REMOVAL
            return true;
        default:
            return false;
    }
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    (void) lun;
    
    if (lba >= BLOCK_COUNT) return -1;
    if (offset >= BLOCK_SIZE) return -1;
    
    uint32_t addr = lba * BLOCK_SIZE + offset;
    if (addr + bufsize > sizeof(ram_disk)) return -1;
    
    memcpy(buffer, &ram_disk[addr], bufsize);
    return bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    (void) lun;
    
    if (lba >= BLOCK_COUNT) return -1;
    if (offset >= BLOCK_SIZE) return -1;
    
    uint32_t addr = lba * BLOCK_SIZE + offset;
    if (addr + bufsize > sizeof(ram_disk)) return -1;
    
    memcpy(&ram_disk[addr], buffer, bufsize);
    return bufsize;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void) lun;
    
    const char vid[] = "Pico";
    const char pid[] = "Mass Storage";
    const char rev[] = "1.0";
    
    memcpy(vendor_id, vid, strlen(vid));
    memcpy(product_id, pid, strlen(pid));
    memcpy(product_rev, rev, strlen(rev));
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size) {
    (void) lun;
    *block_count = BLOCK_COUNT;
    *block_size = BLOCK_SIZE;
}