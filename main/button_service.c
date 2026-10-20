/** Korvo-2 V3.1 ADC 电阻按键；业务动作统一由 app_main 消费。 */
#include "button_service.h"
#include "button_policy.h"
#include "board_config.h"
#include "event_bus.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char *TAG = "button_service";
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_calibration;
static TaskHandle_t s_task;

static void publish_action(button_action_t action)
{
    event_type_t event;
    switch (action) {
    case BUTTON_ACTION_VOL_DOWN: event = EV_BUTTON_VOLUME_DOWN; break;
    case BUTTON_ACTION_VOL_UP: event = EV_BUTTON_VOLUME_UP; break;
    case BUTTON_ACTION_TALK: event = EV_BUTTON_TALK; break;
    case BUTTON_ACTION_END_TALK: event = EV_BUTTON_END_TALK; break;
    case BUTTON_ACTION_PROVISION: event = EV_BUTTON_PROVISION; break;
    default: return;
    }
    if (event_bus_publish(event, NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "Button event dropped: bus full (event=%d)", event);
    }
}

static void button_task(void *arg)
{
    (void)arg;
    button_policy_t policy = button_policy_initial();
    while (1) {
        int total = 0, raw = 0, mv = 0;
        bool valid = true;
        for (int i = 0; i < 4; ++i) {
            if (adc_oneshot_read(s_adc, BSP_ADC_KEY_CHANNEL, &raw) != ESP_OK) {
                valid = false;
                break;
            }
            total += raw;
        }
        if (valid) valid = adc_cali_raw_to_voltage(s_calibration, total / 4, &mv) == ESP_OK;
        button_id_t key = valid ? button_from_mv(mv) : BUTTON_INVALID;
        publish_action(button_policy_feed(&policy, key, (uint32_t)(esp_timer_get_time() / 1000)));
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

esp_err_t button_service_init(void)
{
    if (s_task) return ESP_OK;
    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    esp_err_t err = adc_oneshot_new_unit(&unit, &s_adc);
    if (err != ESP_OK) return err;
    adc_oneshot_chan_cfg_t channel = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    err = adc_oneshot_config_channel(s_adc, BSP_ADC_KEY_CHANNEL, &channel);
    if (err != ESP_OK) goto fail;
    adc_cali_curve_fitting_config_t calibration = {
        .unit_id = ADC_UNIT_1, .chan = BSP_ADC_KEY_CHANNEL,
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&calibration, &s_calibration);
    if (err != ESP_OK) goto fail;
    if (xTaskCreate(button_task, "adc_buttons", 3072, NULL, 3, &s_task) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    ESP_LOGI(TAG, "ADC keys ready: REC=talk, SET(3s)=Wi-Fi setup, VOL+/-=volume");
    return ESP_OK;
fail:
    if (s_calibration) adc_cali_delete_scheme_curve_fitting(s_calibration);
    s_calibration = NULL;
    adc_oneshot_del_unit(s_adc);
    s_adc = NULL;
    return err;
}
