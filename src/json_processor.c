#include "json_processor.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"

#define RX_BUFFER_SIZE 512

typedef struct {
    bool is_initialized;
    
    // Serial buffer
    char rx_buffer[RX_BUFFER_SIZE];
    int rx_index;
    
    // Health data
    health_data_t current_health;
    uint32_t last_data_time;
    uint32_t sample_count;
    bool is_connected;
    
    // Auto-post configuration
    bool enable_auto_post;
    uint32_t min_post_interval_ms;
    uint32_t last_post_time;
    
    // Callbacks
    void (*on_data_received)(health_data_t* data);
    void (*on_post_trigger)(health_data_t* data);
} json_processor_state_t;

static json_processor_state_t json_state = {
    .is_initialized = false,
    .rx_index = 0,
    .current_health = {0},
    .last_data_time = 0,
    .sample_count = 0,
    .is_connected = false,
    .enable_auto_post = false,
    .min_post_interval_ms = 0,
    .last_post_time = 0,
    .on_data_received = NULL,
    .on_post_trigger = NULL
};

static void parse_json_data(char *json)
{
    char *cpu_pos = strstr(json, "\"cpu\":");
    char *mem_pos = strstr(json, "\"memory\":");
    char *disk_pos = strstr(json, "\"disk\":");
    char *net_in_pos = strstr(json, "\"net_in\":");
    char *net_out_pos = strstr(json, "\"net_out\":");
    char *proc_pos = strstr(json, "\"processes\":");

    // Mark as connected on first data received
    if (!json_state.is_connected) {
        json_state.is_connected = true;
        printf("[JSON PROCESSOR] Connected - starting sample counter\n");
    }

    // Parse all available fields
    if (cpu_pos) json_state.current_health.cpu = atof(cpu_pos + 6);
    if (mem_pos) json_state.current_health.memory = atof(mem_pos + 10);
    if (disk_pos) json_state.current_health.disk = atof(disk_pos + 7);
    if (net_in_pos) json_state.current_health.net_in = atof(net_in_pos + 10);
    if (net_out_pos) json_state.current_health.net_out = atof(net_out_pos + 11);
    if (proc_pos) json_state.current_health.processes = atoi(proc_pos + 13);

    json_state.current_health.valid = true;
    json_state.last_data_time = to_ms_since_boot(get_absolute_time());
    json_state.sample_count++;

    // Print status
    printf("\r[%3lu] CPU:%5.1f%% MEM:%5.1f%% DSK:%5.1f%%\n",
           json_state.sample_count,
           json_state.current_health.cpu,
           json_state.current_health.memory,
           json_state.current_health.disk);
    fflush(stdout);

    // Trigger callbacks
    if (json_state.on_data_received) {
        json_state.on_data_received(&json_state.current_health);
    }

    // Handle auto-post if enabled
    if (json_state.enable_auto_post && json_state.on_post_trigger) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - json_state.last_post_time >= json_state.min_post_interval_ms) {
            json_state.on_post_trigger(&json_state.current_health);
            json_state.last_post_time = now;
        }
    }
}

bool json_processor_init(const json_processor_config_t *config)
{
    if (json_state.is_initialized) {
        printf("JSON Processor: Already initialized\n");
        return true;
    }
    
    if (!config) {
        printf("JSON Processor: Invalid configuration\n");
        return false;
    }
    
    // Initialize state
    memset(json_state.rx_buffer, 0, RX_BUFFER_SIZE);
    json_state.rx_index = 0;
    memset(&json_state.current_health, 0, sizeof(health_data_t));
    json_state.last_data_time = 0;
    json_state.sample_count = 0;
    json_state.is_connected = false;
    
    // Store configuration
    json_state.enable_auto_post = config->enable_auto_post;
    json_state.min_post_interval_ms = config->min_post_interval_ms;
    json_state.last_post_time = 0;
    json_state.on_data_received = config->on_data_received;
    json_state.on_post_trigger = config->on_post_trigger;
    
    json_state.is_initialized = true;
    
    printf("JSON Processor: Initialized successfully\n");
    printf("  Auto-post: %s\n", config->enable_auto_post ? "ENABLED" : "DISABLED");
    if (config->enable_auto_post) {
        printf("  Min post interval: %lu ms\n", config->min_post_interval_ms);
    }
    
    return true;
}

void json_processor_process_char(int c)
{
    if (!json_state.is_initialized) {
        return;
    }
    
    // Handle line endings (CR or LF)
    if (c == '\r' || c == '\n') {
        // Null-terminate the buffer
        if (json_state.rx_index < RX_BUFFER_SIZE) {
            json_state.rx_buffer[json_state.rx_index] = '\0';
        } else {
            json_state.rx_buffer[RX_BUFFER_SIZE - 1] = '\0';
        }
        
        // Process if buffer contains JSON (starts with '{')
        if (json_state.rx_index > 0 && json_state.rx_buffer[0] == '{') {
            parse_json_data(json_state.rx_buffer);
        }
        
        // Reset buffer
        json_state.rx_index = 0;
    }
    // Add character to buffer
    else if (json_state.rx_index < RX_BUFFER_SIZE - 1) {
        json_state.rx_buffer[json_state.rx_index++] = c;
    }
    // Buffer overflow - silently discard (buffer will reset on next line ending)
}

const health_data_t* json_processor_get_health_data(void)
{
    return &json_state.current_health;
}

uint32_t json_processor_get_sample_count(void)
{
    return json_state.sample_count;
}

bool json_processor_is_connected(void)
{
    return json_state.is_connected;
}

uint32_t json_processor_get_time_since_last_data(void)
{
    if (!json_state.is_connected || json_state.last_data_time == 0) {
        return 0;
    }
    
    uint32_t now = to_ms_since_boot(get_absolute_time());
    return now - json_state.last_data_time;
}