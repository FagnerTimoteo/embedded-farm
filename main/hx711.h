#pragma once
#include "driver/gpio.h"
#include <stdint.h>

typedef struct {
    gpio_num_t dout;
    gpio_num_t sck;
    int32_t offset;
    float scale;
} hx711_t;

void hx711_init(hx711_t *hx, gpio_num_t dout, gpio_num_t sck);
int32_t hx711_read(hx711_t *hx);
void hx711_tare(hx711_t *hx, int samples);
float hx711_get_units(hx711_t *hx, int samples);
