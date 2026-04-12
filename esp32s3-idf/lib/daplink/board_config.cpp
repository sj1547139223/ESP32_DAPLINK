#include "board_config.h"

#include "esp_check.h"

namespace board_config {

void configure_swd_gpio()
{
    const gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << kSwclkPin) | (1ULL << kSwdOutputPin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out_cfg));

    const gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << kSwdInputPin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in_cfg));

    // nRST: open-drain output, idle HIGH (deasserted)
    const gpio_config_t nrst_cfg = {
        .pin_bit_mask = (1ULL << kNrstPin),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&nrst_cfg));
    gpio_set_level(kNrstPin, 1);

    gpio_set_level(kSwclkPin, 1);
    gpio_set_level(kSwdOutputPin, 1);
}

void configure_led_gpio()
{
    const gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << kLedRedPin) | (1ULL << kLedGreenPin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led_cfg));
    gpio_set_level(kLedRedPin, 0);
    gpio_set_level(kLedGreenPin, 0);
}

void set_led_red(bool on)
{
    gpio_set_level(kLedRedPin, on ? 1 : 0);
}

void set_led_green(bool on)
{
    gpio_set_level(kLedGreenPin, on ? 1 : 0);
}

} // namespace board_config
