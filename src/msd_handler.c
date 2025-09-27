#include "msd_handler.h"
#include <string.h>

#define BLOCK_SIZE  512
#define BLOCK_COUNT 8192  // 4MB storage

// RAM disk storage
static uint8_t ram_disk[BLOCK_SIZE * BLOCK_COUNT] __attribute__((aligned(4)));

void msd_init(void) {
    // Initialize RAM disk with zeros
    memset(ram_disk, 0, sizeof(ram_disk));
    
    // Create a simple FAT signature to make it recognizable
    ram_disk[510] = 0x55;
    ram_disk[511] = 0xAA;
    
    printf("MSD: Initialized %lu KB RAM disk\n", sizeof(ram_disk) / 1024);
}

void msd_task(void) {
    // Periodic MSD tasks can go here
}

bool msd_is_ready(void) {
    return tud_mounted() && tud_msc_ready();
}

//--------------------------------------------------------------------+
// TinyUSB MSC Callbacks
//--------------------------------------------------------------------+

// Invoked when received SCSI command not in built-in list below
// - READ_CAPACITY10, READ_FORMAT_CAPACITY, INQUIRY, MODE_SENSE6, REQUEST_SENSE
// - READ10 and WRITE10 have their own callbacks
bool tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize) {
    // Parse SCSI command
    switch (scsi_cmd[0]) {
        case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
            // Sync the disk if needed
            return true;
            
        default:
            // Unsupported command
            return false;
    }
}

// Callback invoked when received READ10 command.
// Read from storage and copy to buffer
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    (void)lun;
    
    if (lba >= BLOCK_COUNT) return -1;
    if (offset >= BLOCK_SIZE) return -1;
    
    uint32_t addr = lba * BLOCK_SIZE + offset;
    if (addr + bufsize > sizeof(ram_disk)) return -1;
    
    memcpy(buffer, &ram_disk[addr], bufsize);
    return bufsize;
}

// Callback invoked when received WRITE10 command.
// Write data from buffer to storage
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    (void)lun;
    
    if (lba >= BLOCK_COUNT) return -1;
    if (offset >= BLOCK_SIZE) return -1;
    
    uint32_t addr = lba * BLOCK_SIZE + offset;
    if (addr + bufsize > sizeof(ram_disk)) return -1;
    
    memcpy(&ram_disk[addr], buffer, bufsize);
    return bufsize;
}

// Callback invoked when received an SCSI INQUIRY command
// Fill vendor id, product id and revision with string up to 8, 16, 4 characters respectively
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    
    const char vid[] = "Pico";
    const char pid[] = "Mass Storage";
    const char rev[] = "1.0";
    
    memcpy(vendor_id, vid, strlen(vid));
    memcpy(product_id, pid, strlen(pid));
    memcpy(product_rev, rev, strlen(rev));
}

// Callback invoked when received READ CAPACITY (10) command
// Return disk capacity in block size and number of blocks
void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size) {
    (void)lun;
    *block_count = BLOCK_COUNT;
    *block_size = BLOCK_SIZE;
}

// Callback invoked when received MODE SENSE (6) command
// Return disk configuration
bool tud_msc_mode_sense_cb(uint8_t lun, uint8_t page_control, uint8_t page_code, void* buffer, uint16_t bufsize) {
    (void)lun;
    (void)page_control;
    (void)page_code;
    (void)buffer;
    (void)bufsize;
    return false;  // Use default
}

// Callback invoked when received REQUEST SENSE command
// Return sense data
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun;
    (void)power_condition;
    return true;
}