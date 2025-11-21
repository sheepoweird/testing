#ifndef JSON_PROCESSOR_H
#define JSON_PROCESSOR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float cpu;
    float memory;
    float disk;
    float net_in;
    float net_out;
    int processes;
    bool valid;
} health_data_t;

typedef struct {
    bool enable_auto_post;              // Enable automatic webhook posting on data receipt
    uint32_t min_post_interval_ms;      // Minimum interval between posts (ms)
    void (*on_data_received)(health_data_t* data);  // Optional callback when data is parsed
    void (*on_post_trigger)(health_data_t* data);   // Optional callback to trigger webhook post
} json_processor_config_t;

bool json_processor_init(const json_processor_config_t *config);

void json_processor_process_char(int c);

const health_data_t* json_processor_get_health_data(void);

uint32_t json_processor_get_sample_count(void);

bool json_processor_is_connected(void);

uint32_t json_processor_get_time_since_last_data(void);

void json_processor_reset(void);

#endif // JSON_PROCESSOR_H