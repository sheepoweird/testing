#include "hw_config.h"

static spi_t spi = {
    .hw_inst = spi1,  // GP10-15 are on SPI1 peripheral
    .miso_gpio = 12,  // GP12
    .mosi_gpio = 11,  // GP11
    .sck_gpio  = 10,  // GP10
    .baud_rate = 12 * 1000 * 1000, // 12 MHz safe default
    .set_drive_strength = true,
    .mosi_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
    .sck_gpio_drive_strength  = GPIO_DRIVE_STRENGTH_4MA,
};

static sd_spi_if_t spi_if = {
    .spi = &spi,
    .ss_gpio = 15,   // GP15 as CS
};

static sd_card_t sd_card = {
    .type = SD_IF_SPI,
    .spi_if_p = &spi_if,
    .use_card_detect = false,  // Maker Pi Pico slot has no CD pin
};

/* ********************************************************************** */

size_t sd_get_num(void) {
    return 1;
}

sd_card_t *sd_get_by_num(size_t num) {
    if (0 == num)
        return &sd_card;
    else
        return NULL;
}