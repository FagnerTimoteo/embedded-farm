#include "hx711.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

void hx711_init(hx711_t *hx, gpio_num_t dout, gpio_num_t sck)
{
    hx->dout = dout;
    hx->sck = sck;
    hx->offset = 0;
    hx->scale = 1.0f;

    gpio_set_direction(dout, GPIO_MODE_INPUT);
    gpio_set_direction(sck, GPIO_MODE_OUTPUT);
    gpio_set_level(sck, 0);
}

int32_t hx711_read(hx711_t *hx)
{
    int32_t value = 0;

    while (gpio_get_level(hx->dout)); // espera pronto

    for (int i = 0; i < 24; i++) {
        gpio_set_level(hx->sck, 1);
        esp_rom_delay_us(1);
        value = (value << 1) | gpio_get_level(hx->dout);
        gpio_set_level(hx->sck, 0);
        esp_rom_delay_us(1);
    }

    gpio_set_level(hx->sck, 1); // ganho 128
    esp_rom_delay_us(1);
    gpio_set_level(hx->sck, 0);

    if (value & 0x800000)
        value |= ~0xFFFFFF;

    return value;
}

void hx711_tare(hx711_t *hx, int samples)
{
    int64_t sum = 0;
    for (int i = 0; i < samples; i++) {
        sum += hx711_read(hx);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    hx->offset = sum / samples;
}

float hx711_get_units(hx711_t *hx, int samples)
{
    int64_t sum = 0;
    for (int i = 0; i < samples; i++) {
        sum += hx711_read(hx);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ((sum / samples) - hx->offset) / hx->scale;
}
