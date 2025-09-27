#ifndef _HID_TEST_H_
#define _HID_TEST_H_

#include "pico/stdlib.h"

void hid_test_init(void);
void hid_test_task(void);
void send_hid_report(uint8_t keycode);

#endif