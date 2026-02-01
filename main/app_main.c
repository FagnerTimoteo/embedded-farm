#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_rom_sys.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/adc.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_wifi.h"
#include "esp_log.h"

#include "mqtt_client.h"
#include "cJSON.h"

#include <math.h>
#include <inttypes.h>
#include "esp_system.h"
#include "esp_mac.h"

#include "hx711.h"
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"

static int dht11_read(float *temperature, float *humidity);

// Defines
#define TAG "GRANJA"
#define BROKER_URI "mqtt://192.168.2.164:1883"
#define WIFI_SSID "EspIdfWrt"
#define WIFI_PASS "12345678"
#define PUMP_MAX_ON_TIME_SEC  10   // 10 segundos
#define HX_SAMPLES 10

// Variáveis globais
static hx711_t balanca;
static bool fan_on = false;
static bool light_on = false;

static bool bomba_ligada = false;
static int64_t pump_on_timestamp = 0;
static bool pump_timeout_alert_sent = false;
static volatile bool boia_acionada = false;
static SemaphoreHandle_t boia_sem;
static bool mqtt_started = false;
static bool mqtt_connected = false;
static float hx_buffer[HX_SAMPLES];
static int hx_index = 0;
static bool hx_full = false;

// Pinos
// Sensores
#define DHT_GPIO           GPIO_NUM_18
#define TERMISTOR_ADC      ADC1_CHANNEL_6   // GPIO34
#define LDR_ADC            ADC1_CHANNEL_7   // GPIO35
#define BOIA_GPIO          GPIO_NUM_19
#define HX711_DOUT         GPIO_NUM_32
#define HX711_SCK          GPIO_NUM_33

// Atuadores
#define RELE_BOMBA         GPIO_NUM_21
#define RELE_LUZ           GPIO_NUM_22
#define RELE_VENTILADOR    GPIO_NUM_23

// dht11
static SemaphoreHandle_t dht_mutex = NULL;
static float dht_temperature = NAN;
static float dht_humidity    = NAN;
static bool  dht_valid       = false;

// Enums
typedef enum {
    MODE_AUTO,
    MODE_MANUAL
} control_mode_t;

typedef enum {
    LIGHT_LDR = 0,
    LIGHT_SCHEDULE
} light_control_t;

// Config structs
typedef struct {
    light_control_t control;
    int ldr_threshold;
    int on_hour;
    int on_min;
    int off_hour;
    int off_min;
} light_config_t;

typedef struct {
    float min;
    float max;
} temperature_config_t;

typedef struct {
    float min_weight;
    float max_weight;
} ration_config_t;

typedef struct {
    light_config_t light;
    temperature_config_t temperature;
    ration_config_t ration;
    int save_interval;
} system_config_t;


typedef enum {
    RATION_OK = 0,
    RATION_LOW,
    RATION_HIGH
} ration_state_t;

static control_mode_t system_mode = MODE_AUTO;
static ration_state_t ration_state = RATION_OK;

// Configuração global inicial
static system_config_t config = {
    .light = {
        .control = LIGHT_LDR,
        .ldr_threshold = 300,
        .on_hour = 19,
        .on_min = 0,
        .off_hour = 5,
        .off_min = 0
    },
    .temperature = {
        .min = 23.0,
        .max = 24.0
    },
    .ration = {
        .min_weight = 300,
        .max_weight = 1000
    },
    .save_interval = 10
};


static esp_mqtt_client_handle_t mqtt_client = NULL;

// Time initialization
#include "esp_sntp.h" // garanta que está incluído no topo do arquivo

void init_time(void) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

float hx711_filtered_read(hx711_t *hx)
{
    const int N = 15;
    float samples[N];

    for (int i = 0; i < N; i++) {
        samples[i] = hx711_get_units(hx, 1);
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    // ordenar (bubble simples)
    for (int i = 0; i < N - 1; i++) {
        for (int j = 0; j < N - i - 1; j++) {
            if (samples[j] > samples[j + 1]) {
                float t = samples[j];
                samples[j] = samples[j + 1];
                samples[j + 1] = t;
            }
        }
    }

    // descarta extremos (3 menores + 3 maiores)
    float sum = 0;
    int count = 0;

    for (int i = 3; i < N - 3; i++) {
        sum += samples[i];
        count++;
    }

    return (sum / count) * 1000.0f;
}

// Manual
static void handle_manual_command(const char *payload) {
    cJSON *json = cJSON_Parse(payload);
    if (!json) return;

    cJSON *actuator = cJSON_GetObjectItem(json, "actuator");
    cJSON *state    = cJSON_GetObjectItem(json, "state");

    if (cJSON_IsString(actuator) && cJSON_IsString(state)) {

        system_mode = MODE_MANUAL;

        bool turn_on = strcmp(state->valuestring, "ON") == 0;

        if (strcmp(actuator->valuestring, "fan") == 0) {
            gpio_set_level(RELE_VENTILADOR, turn_on ? 0 : 1);
            fan_on = turn_on;
        }
        else if (strcmp(actuator->valuestring, "light") == 0) {
            gpio_set_level(RELE_LUZ, turn_on ? 0 : 1);
            light_on = turn_on;
        }
    }

    cJSON_Delete(json);
}

// Mqtt event handler
static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    switch (event->event_id) {

        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT CONECTADO");
            mqtt_connected = true;
            esp_mqtt_client_subscribe(event->client, "granja/config", 0);
            esp_mqtt_client_subscribe(event->client, "granja/manual", 0);
            esp_mqtt_client_subscribe(event->client, "granja/mode", 0);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT DESCONECTADO");
            mqtt_connected = false;
            break;

        case MQTT_EVENT_DATA: {
            char topic[event->topic_len + 1];
            char payload[event->data_len + 1];

            memcpy(topic, event->topic, event->topic_len);
            topic[event->topic_len] = 0;

            memcpy(payload, event->data, event->data_len);
            payload[event->data_len] = 0;

            ESP_LOGI(TAG, "TOPICO: %s", topic);
            ESP_LOGI(TAG, "PAYLOAD: %s", payload);

            if (strcmp(topic, "granja/manual") == 0) {
                handle_manual_command(payload);
                break;
            }

            if (strcmp(topic, "granja/mode") == 0) {
                cJSON *json = cJSON_Parse(payload);
                if (!json) {
                    ESP_LOGE(TAG, "JSON INVALIDO");
                    return ESP_OK;
                }

                cJSON *mode = cJSON_GetObjectItem(json, "mode");

                if (cJSON_IsString(mode) && strcmp(mode->valuestring, "auto") == 0) {
                    system_mode = MODE_AUTO;
                    ESP_LOGI(TAG, "Sistema voltou para modo AUTOMÁTICO");
                }

                cJSON_Delete(json);
            }


            if (strcmp(topic, "granja/config") != 0)
                break;

            cJSON *json = cJSON_Parse(payload);
            if (!json) {
                ESP_LOGE(TAG, "JSON INVALIDO");
                break;
            }

            // Luz
            cJSON *light = cJSON_GetObjectItem(json, "light");
            if (cJSON_IsObject(light)) {

                cJSON *control = cJSON_GetObjectItem(light, "control");
                if (cJSON_IsString(control)) {
                    if (strcmp(control->valuestring, "ldr") == 0)
                        config.light.control = LIGHT_LDR;
                    else if (strcmp(control->valuestring, "schedule") == 0)
                        config.light.control = LIGHT_SCHEDULE;
                }

                cJSON *ldr = cJSON_GetObjectItem(light, "ldrThreshold");
                if (cJSON_IsNumber(ldr))
                    config.light.ldr_threshold = ldr->valueint;

                cJSON *onTime = cJSON_GetObjectItem(light, "onTime");
                if (cJSON_IsString(onTime))
                    sscanf(onTime->valuestring, "%d:%d",
                           &config.light.on_hour,
                           &config.light.on_min);

                cJSON *offTime = cJSON_GetObjectItem(light, "offTime");
                if (cJSON_IsString(offTime))
                    sscanf(offTime->valuestring, "%d:%d",
                           &config.light.off_hour,
                           &config.light.off_min);
            }

            // Temperatura
            cJSON *temperature = cJSON_GetObjectItem(json, "temperature");
            if (cJSON_IsObject(temperature)) {

                cJSON *min = cJSON_GetObjectItem(temperature, "min");
                cJSON *max = cJSON_GetObjectItem(temperature, "max");

                if (cJSON_IsNumber(min))
                    config.temperature.min = min->valuedouble;

                if (cJSON_IsNumber(max))
                    config.temperature.max = max->valuedouble;
            }

            // Ração
            cJSON *ration = cJSON_GetObjectItem(json, "ration");
            if (cJSON_IsObject(ration)) {

                cJSON *minW = cJSON_GetObjectItem(ration, "minWeight");
                cJSON *maxW = cJSON_GetObjectItem(ration, "maxWeight");

                if (cJSON_IsNumber(minW))
                    config.ration.min_weight = minW->valuedouble;

                if (cJSON_IsNumber(maxW))
                    config.ration.max_weight = maxW->valuedouble;
            }

            // Save Interval
            cJSON *saveInterval = cJSON_GetObjectItem(json, "saveInterval");
            if (cJSON_IsNumber(saveInterval) && saveInterval->valueint > 0) {
                config.save_interval = saveInterval->valueint;
            }

            ESP_LOGI(TAG,
                "CFG -> Light=%s LDR=%d On=%02d:%02d Off=%02d:%02d | "
                "Temp(%.1f-%.1f) | Ration(%.1f-%.1f) | SaveInterval=%d",
                config.light.control == LIGHT_LDR ? "LDR" : "SCHEDULE",
                config.light.ldr_threshold,
                config.light.on_hour, config.light.on_min,
                config.light.off_hour, config.light.off_min,
                config.temperature.min,
                config.temperature.max,
                config.ration.min_weight,
                config.ration.max_weight,
                config.save_interval
            );

            cJSON_Delete(json);
            break;
        }

        default:
            break;
    }

    return ESP_OK;
}

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t id, void *data) {
    mqtt_event_handler_cb(data);
}

// MQTT publish function
static void mqtt_publish_sensors(float Temp, float humidity,
    int luminosity, bool boia_acionada, float ration_weight) {
    if (!mqtt_client) return;

    char payload[256];

    snprintf(payload, sizeof(payload),
        "{"
        "\"temperature\":%.2f,"
        "\"humidity\":%.1f,"
        "\"luminosity\":%d,"
        "\"rationWeight\":%.2f,"
        "\"waterLevel\":%s"
        "}",
        Temp,
        humidity,
        luminosity,
        ration_weight,
        boia_acionada ? "true" : "false"
    );

    esp_mqtt_client_publish(mqtt_client, "granja/sensors", payload, 0, 1, 0);
}

// Termistor calcular constantes
static const double beta = 3600.0;
static const double r0   = 10000.0;
static const double t0   = 273.15 + 25.0;
static const double vcc  = 3.3; 
static const double resistor = 10000.0;
static const int    nAmostras = 5;

int time_to_minutes(const char *timeStr) {
    int h, m;
    sscanf(timeStr, "%d:%d", &h, &m);
    return h * 60 + m;
}

// Alerta e automações
void check_ration_alert(float weight) {

    ration_state_t new_state = RATION_OK;

    if (weight < config.ration.min_weight) {
        new_state = RATION_LOW;
    }
    else if (weight > config.ration.max_weight) {
        new_state = RATION_HIGH;
    }
    else {
        new_state = RATION_OK;
    }

    // só age se mudou de estado
    if (new_state == ration_state)
        return;

    ration_state = new_state;

    char alert[256];

    if (ration_state == RATION_LOW) {
        snprintf(alert, sizeof(alert),
            "{"
            "\"type\":\"RATION_LOW\","
            "\"currentWeight\":%.2f,"
            "\"minWeight\":%.2f"
            "}",
            weight,
            config.ration.min_weight
        );

        ESP_LOGW(TAG, "ALERTA: Ração abaixo do mínimo");

        esp_mqtt_client_publish(
            mqtt_client,
            "granja/alerts",
            alert, 0, 1, 0
        );
    }
    else if (ration_state == RATION_HIGH) {
        snprintf(alert, sizeof(alert),
            "{"
            "\"type\":\"RATION_HIGH\","
            "\"currentWeight\":%.2f,"
            "\"maxWeight\":%.2f"
            "}",
            weight,
            config.ration.max_weight
        );

        ESP_LOGW(TAG, "ALERTA: Ração acima do máximo");

        esp_mqtt_client_publish(
            mqtt_client,
            "granja/alerts",
            alert, 0, 1, 0
        );
    }
    else {
        ESP_LOGI(TAG, "Ração normalizada");
    }
}

void control_fan_by_temperature(float avgTemp) {

    // Liga quando atinge o máximo
    if (!fan_on && avgTemp >= config.temperature.max) {
        gpio_set_level(RELE_VENTILADOR, 0); // relé ativo em LOW
        fan_on = true;
        ESP_LOGI(TAG, "VENTILADOR LIGADO (%.2f °C)", avgTemp);
    }

    // Desliga quando atinge o mínimo
    else if (fan_on && avgTemp <= config.temperature.min) {
        gpio_set_level(RELE_VENTILADOR, 1);
        fan_on = false;
        ESP_LOGI(TAG, "VENTILADOR DESLIGADO (%.2f °C)", avgTemp);
    }
}

void control_light_by_schedule(void) {

    if (config.light.control != LIGHT_SCHEDULE) {
        return;
    }

    // Hora atual
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    int nowMin =
        timeinfo.tm_hour * 60 +
        timeinfo.tm_min;

    int onMin =
        config.light.on_hour * 60 +
        config.light.on_min;

    int offMin =
        config.light.off_hour * 60 +
        config.light.off_min;

    bool shouldBeOn = false;

    // Caso normal (ex: 08:00 → 18:00)
    if (onMin < offMin) {
        shouldBeOn = (nowMin >= onMin && nowMin < offMin);
    }
    // Caso cruzando meia-noite (ex: 18:00 → 06:00)
    else {
        shouldBeOn = (nowMin >= onMin || nowMin < offMin);
    }

    if (shouldBeOn && !light_on) {
        gpio_set_level(RELE_LUZ, 0); // relé ativo em LOW
        light_on = true;
        ESP_LOGI(TAG, "LUZ LIGADA (horário)");
    }
    else if (!shouldBeOn && light_on) {
        gpio_set_level(RELE_LUZ, 1);
        light_on = false;
        ESP_LOGI(TAG, "LUZ DESLIGADA (horário)");
    }
}

void control_light_by_ldr(int ldr_value) {
    int th_on = config.light.ldr_threshold;
    int th_off = th_on + 50; // hysteresis

    if (!light_on && ldr_value < th_on) {
        gpio_set_level(RELE_LUZ, 0); // liga (relay ativo em LOW)
        light_on = true;
        ESP_LOGI(TAG, "LUZ LIGADA (LDR=%d)", ldr_value);
    }
    else if (light_on && ldr_value > th_off) {
        gpio_set_level(RELE_LUZ, 1); // desliga
        light_on = false;
        ESP_LOGI(TAG, "LUZ DESLIGADA (LDR=%d)", ldr_value);
    }
}

void check_pump_timeout(void) {

    if (!bomba_ligada)
        return;

    int64_t now = esp_timer_get_time() / 1000000;

    if (!pump_timeout_alert_sent &&
        (now - pump_on_timestamp) >= PUMP_MAX_ON_TIME_SEC) {

        char alert[128];

        snprintf(alert, sizeof(alert),
            "{"
            "\"type\":\"PUMP_TIMEOUT\","
            "\"onTime\":%lld"
            "}",
            now - pump_on_timestamp
        );

        ESP_LOGW(TAG, "ALERTA: Bomba ligada por muito tempo!");

        esp_mqtt_client_publish(
            mqtt_client,
            "granja/alerts",
            alert,
            0, 1, 0
        );

        pump_timeout_alert_sent = true;
    }
}

void control_water_pump(bool boia_acionada) {

    if (!boia_acionada && !bomba_ligada) {
        gpio_set_level(RELE_BOMBA, 0);
        bomba_ligada = true;

        pump_on_timestamp = esp_timer_get_time() / 1000000;
        pump_timeout_alert_sent = false;
        ESP_LOGW(TAG, "BOMBA LIGADA");
    }
    else if (boia_acionada && bomba_ligada) {
        gpio_set_level(RELE_BOMBA, 1);
        bomba_ligada = false;

        ESP_LOGI(TAG, "BOMBA DESLIGADA");
    }
}

// dht 11 task
static void dht_task(void *pv)
{
    float temp, hum;

    while (1) {
        int res = dht11_read(&temp, &hum);

        if (res == 0) {
            if (xSemaphoreTake(dht_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                dht_temperature = temp;
                dht_humidity    = hum;
                dht_valid       = true;
                xSemaphoreGive(dht_mutex);
            }

            ESP_LOGI("DHT", "OK -> T=%.1f°C H=%.1f%%", temp, hum);
        } else {
            ESP_LOGW("DHT", "Erro leitura (%d)", res);
        }

        // DHT11 exige >= 2s entre leituras
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static int dht11_read(float *temperature, float *humidity) {
    uint8_t data[5] = {0};

    if (!dht_mutex) return -99;

    // tenta pegar o mutex por até 500 ms
    if (xSemaphoreTake(dht_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return -1; // busy / não consegui exclusão
    }

    // start signal
    gpio_set_direction(DHT_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT_GPIO, 0);
    esp_rom_delay_us(20000);

    gpio_set_level(DHT_GPIO, 1);
    esp_rom_delay_us(40);
    gpio_set_direction(DHT_GPIO, GPIO_MODE_INPUT);

    uint32_t timeout = 0;
    // espera resposta inicial (low)
    while (gpio_get_level(DHT_GPIO)) {
        if (++timeout > 10000) { xSemaphoreGive(dht_mutex); return -2; }
        esp_rom_delay_us(1);
    }

    timeout = 0;
    // espera resposta high
    while (!gpio_get_level(DHT_GPIO)) {
        if (++timeout > 10000) { xSemaphoreGive(dht_mutex); return -3; }
        esp_rom_delay_us(1);
    }

    timeout = 0;
    // espera sinal de 1ª transição
    while (gpio_get_level(DHT_GPIO)) {
        if (++timeout > 10000) { xSemaphoreGive(dht_mutex); return -4; }
        esp_rom_delay_us(1);
    }

    // ler 40 bits
    for (int i = 0; i < 40; i++) {
        // aguarda início do bit (low)
        timeout = 0;
        while (!gpio_get_level(DHT_GPIO)) {
            if (++timeout > 10000) { xSemaphoreGive(dht_mutex); return -5; }
            esp_rom_delay_us(1);
        }

        // mede duração do nível alto
        uint32_t t = 0;
        while (gpio_get_level(DHT_GPIO)) {
            esp_rom_delay_us(1);
            if (++t > 200) break;
        }

        data[i / 8] <<= 1;
        if (t > 40) data[i / 8] |= 1;
    }

    xSemaphoreGive(dht_mutex);

    if (((data[0] + data[1] + data[2] + data[3]) & 0xFF) != data[4])
        return -6; // checksum

    *humidity    = data[0];
    *temperature = data[2];
    return 0;
}

// Boia task
static void boia_task(void *pv) {
    while (1) {
        // Espera evento da boia
        if (xSemaphoreTake(boia_sem, portMAX_DELAY) == pdTRUE) {

            // pequena filtragem (anti-ruído)
            vTaskDelay(pdMS_TO_TICKS(20));

            bool estado = gpio_get_level(BOIA_GPIO) == 0;

            boia_acionada = estado;

            control_water_pump(boia_acionada);

            ESP_LOGI(TAG, "Boia mudou -> %s",
                boia_acionada ? "ACIONADA" : "DESACIONADA");
        }
    }
}

static void IRAM_ATTR boia_isr_handler(void *arg) {
    boia_acionada = gpio_get_level(BOIA_GPIO) == 0; // ativo em LOW
    xSemaphoreGiveFromISR(boia_sem, NULL);
}

// Task de sensores
static void sensor_task(void *pv) {
    float temp_dht = NAN, hum_dht = NAN;
    bool dht_ok = false;

    while (1) {
        // -------- HX711 --------
        float weight = hx711_filtered_read(&balanca);
        if (fabs(weight) < 2.0f)
            weight = 0;
        if (abs(weight) < 2)
            hx711_tare(&balanca, 10);
        check_ration_alert(weight);
        ESP_LOGI(TAG, "Peso: %.2f g", weight);

        // -------- TERMISTOR --------
        int soma = 0;
        for (int i = 0; i < nAmostras; i++) {
            soma += adc1_get_raw(TERMISTOR_ADC);
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        double adc = soma / (double)nAmostras;
        double v   = (adc / 4095.0) * vcc;
        double rt = resistor * ((vcc - v) / v);

        double tempK = 1.0 / ((1.0 / t0) + (1.0 / beta) * log(rt / r0));
        double tempC = tempK - 273.15;

        ESP_LOGI(TAG, "Termistor: %.2f °C", tempC);

        // Lê o valor do DHT11. Não chama dht11_read diretamente para evitar conflito.
        if (xSemaphoreTake(dht_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            temp_dht = dht_temperature;
            hum_dht = dht_humidity;
            dht_ok = dht_valid;
            xSemaphoreGive(dht_mutex);
        } else {
            dht_ok = false;
        }

        float avgTemp = (float)tempC;
        if (dht_ok && !isnan(temp_dht)) {
            avgTemp = ((float)tempC + temp_dht) / 2.0f;
            ESP_LOGI(TAG, "DHT11 -> Temp: %.1f °C | Umid: %.1f %%", temp_dht, hum_dht);
        } else {
            ESP_LOGW(TAG, "DHT11 sem leitura valida, usando termistor");
        }

        if (system_mode == MODE_AUTO) {
            control_fan_by_temperature(avgTemp);
        }

        // -------- LDR --------
        int ldr_raw = adc1_get_raw(LDR_ADC);
        double ldr_v = (ldr_raw / 4095.0) * vcc;
        ESP_LOGI(TAG, "LDR -> ADC: %d | %.2f V", ldr_raw, ldr_v);

        if (system_mode == MODE_AUTO) {
            if (config.light.control == LIGHT_LDR) {
                control_light_by_ldr(ldr_raw);
            } else if (config.light.control == LIGHT_SCHEDULE) {
                control_light_by_schedule();
            }
        }

        // -------- PUBLICA NO MQTT --------
        mqtt_publish_sensors(avgTemp, dht_ok ? hum_dht : 0.0f, ldr_raw, boia_acionada, weight);

        vTaskDelay(pdMS_TO_TICKS(config.save_interval * 1000));
    }
}

void connect_wifi(void)
{
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}


static void wifi_event_handler(void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
if (event_base == WIFI_EVENT &&
event_id == WIFI_EVENT_STA_START) {
esp_wifi_connect();
}

if (event_base == IP_EVENT &&
event_id == IP_EVENT_STA_GOT_IP) {

ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
ESP_LOGI("WIFI", "IP obtido: " IPSTR, IP2STR(&event->ip_info.ip));

if (!mqtt_started) {
    ESP_LOGI("MQTT", "Iniciando MQTT");
    esp_mqtt_client_start(mqtt_client);
    mqtt_started = true;
}
}

if (event_base == WIFI_EVENT &&
event_id == WIFI_EVENT_STA_DISCONNECTED) {
ESP_LOGW("WIFI", "WiFi desconectado, reconectando...");
esp_wifi_connect();
}
}

// Main
void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Registrar handlers
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifi_event_handler,
        NULL
    ));

    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifi_event_handler,
        NULL
    ));

    connect_wifi();
    // MQTT
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = BROKER_URI
    };

    mqtt_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(
        mqtt_client,
        ESP_EVENT_ANY_ID,
        mqtt_event_handler,
        NULL
    );

    // Mutex DHT11
    dht_mutex = xSemaphoreCreateMutex();
    if (!dht_mutex) {
        ESP_LOGE(TAG, "Falha ao criar mutex do DHT");
    }

    xTaskCreatePinnedToCore(dht_task, "dht_task",4096, NULL, 8, NULL, 1);

    //Sensores
    // HX711
    hx711_init(&balanca, HX711_DOUT, HX711_SCK);
    balanca.scale = 171980.0f;   // fator de calibração
    hx711_tare(&balanca, 20);

    // ADC
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(TERMISTOR_ADC, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(LDR_ADC, ADC_ATTEN_DB_11);

    // GPIO DHT
    gpio_config_t dht_conf = {
        .pin_bit_mask = 1ULL << DHT_GPIO,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&dht_conf);

    // GPIO BOIA
    gpio_config_t boia_conf = {
        .pin_bit_mask = 1ULL << BOIA_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };
    gpio_config(&boia_conf);

    boia_sem = xSemaphoreCreateBinary();
    
    gpio_install_isr_service(0);
    gpio_isr_handler_add(
        BOIA_GPIO,
        boia_isr_handler,
        NULL
    );

    xTaskCreate(boia_task, "boia_task", 2048, NULL, 10,/* prioridade */ NULL);

    //Atuadores
    // GPIO RELÉS
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RELE_LUZ) | (1ULL << RELE_VENTILADOR) | (1ULL << RELE_BOMBA),
        .mode = GPIO_MODE_OUTPUT
    };
    gpio_config(&io_conf);

    // relés desligados (ativo LOW)
    gpio_set_level(RELE_BOMBA, 1);
    gpio_set_level(RELE_LUZ, 1);
    gpio_set_level(RELE_VENTILADOR, 1);

    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
}
