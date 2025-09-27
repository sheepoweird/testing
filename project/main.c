#include <pico/stdlib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <bsp/board.h>
#include <tusb.h>
#include "hardware/gpio.h"


int main(void) {
    // Initialize the board
    board_init();

    // Clear Screen
    printf("\033[2J\033[H");

    // Initialize TinyUSB
    tud_init(BOARD_TUD_RHPORT);

    // Initialize the standard I/O streams
    stdio_init_all();

    // Run the TinyUSB task loop
    while (true) {
        tud_task();
    }
}
