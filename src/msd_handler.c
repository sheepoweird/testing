#include "msd_handler.h"
#include "ff.h"  // FatFs
#include "diskio.h" 

static FATFS fs;
static FIL file;
static bool fs_mounted = false;
static uint8_t ram_disk[MSD_BLOCK_SIZE * MSD_BLOCK_COUNT] __attribute__((aligned(MSD_BLOCK_SIZE)));

void msd_init(void) {
    // Initialize RAM disk with FAT filesystem
    memset(ram_disk, 0, sizeof(ram_disk));
    
    // You can pre-populate with files here if needed
}

void msd_task(void) {
    // Handle any periodic MSD tasks
}

bool msd_ready(void) {
    return tud_msc_ready();
}

// TinyUSB MSD callbacks
bool tud_msd_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void* buffer, uint16_t bufsize) {
    // Handle SCSI commands
    return true;
}

int32_t tud_msd_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    if (lba >= MSD_BLOCK_COUNT) return -1;
    
    uint32_t addr = lba * MSD_BLOCK_SIZE + offset;
    if (addr + bufsize > sizeof(ram_disk)) return -1;
    
    memcpy(buffer, &ram_disk[addr], bufsize);
    return bufsize;
}

int32_t tud_msd_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    if (lba >= MSD_BLOCK_COUNT) return -1;
    
    uint32_t addr = lba * MSD_BLOCK_SIZE + offset;
    if (addr + bufsize > sizeof(ram_disk)) return -1;
    
    memcpy(&ram_disk[addr], buffer, bufsize);
    return bufsize;
}