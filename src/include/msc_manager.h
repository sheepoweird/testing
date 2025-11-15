#ifndef MSC_MANAGER_H
#define MSC_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool enable_mount_callbacks;  // Enable USB mount/unmount event handling
    void (*on_mount)(void);       // Optional callback when USB is mounted
    void (*on_unmount)(void);     // Optional callback when USB is unmounted
} msc_config_t;

bool msc_manager_init(const msc_config_t *config);

bool msc_manager_is_mounted(void);

const char* msc_manager_get_status_string(void);

#endif // MSC_MANAGER_H