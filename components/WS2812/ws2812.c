#include "ws2812.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ws2812";

// ─────────────────────────────────────────────────────────────
//  ESTRUCTURA INTERNA
// ─────────────────────────────────────────────────────────────
struct ws2812_t {
    led_strip_handle_t strip;
    uint16_t           num_leds;
    ws2812_pixel_t    *buf;      // buffer: color + brillo por LED
};

// ─────────────────────────────────────────────────────────────
//  HELPERS INTERNOS
// ─────────────────────────────────────────────────────────────
static inline uint8_t _scale(uint8_t val, uint8_t scale)
{
    return (uint16_t)val * scale / 255;
}

static void _push(struct ws2812_t *dev)
{
    for (uint16_t i = 0; i < dev->num_leds; i++) {
        uint8_t b = dev->buf[i].brightness;
        led_strip_set_pixel(dev->strip, i,
            _scale(dev->buf[i].color.r, b),
            _scale(dev->buf[i].color.g, b),
            _scale(dev->buf[i].color.b, b));
    }
    led_strip_refresh(dev->strip);
}

// ─────────────────────────────────────────────────────────────
//  INIT / DEINIT
// ─────────────────────────────────────────────────────────────
esp_err_t ws2812_init(const ws2812_config_t *cfg, ws2812_handle_t *out)
{
    ESP_RETURN_ON_FALSE(cfg && out, ESP_ERR_INVALID_ARG, TAG, "arg nulo");

    struct ws2812_t *dev = calloc(1, sizeof(struct ws2812_t));
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_NO_MEM, TAG, "sin memoria");

    dev->buf = calloc(cfg->num_leds, sizeof(ws2812_pixel_t));
    if (!dev->buf) { free(dev); return ESP_ERR_NO_MEM; }

    dev->num_leds = cfg->num_leds;

    led_strip_config_t strip_cfg = {
        .strip_gpio_num         = cfg->gpio,
        .max_leds               = cfg->num_leds,
        .led_model              = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out       = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &dev->strip);
    if (err != ESP_OK) { free(dev->buf); free(dev); return err; }

    led_strip_clear(dev->strip);
    *out = dev;

    ESP_LOGI(TAG, "Init OK — %d LEDs en GPIO %d", cfg->num_leds, cfg->gpio);
    return ESP_OK;
}

void ws2812_deinit(ws2812_handle_t h)
{
    if (!h) return;
    led_strip_clear(h->strip);
    led_strip_del(h->strip);
    free(h->buf);
    free(h);
}

// ─────────────────────────────────────────────────────────────
//  CONTROL BÁSICO
// ─────────────────────────────────────────────────────────────
void ws2812_set_pixel(ws2812_handle_t h, uint16_t index,
                      ws2812_color_t color, uint8_t brightness)
{
    if (!h || index >= h->num_leds) return;
    h->buf[index].color      = color;
    h->buf[index].brightness = brightness;
}

void ws2812_fill(ws2812_handle_t h, ws2812_color_t color, uint8_t brightness)
{
    if (!h) return;
    for (uint16_t i = 0; i < h->num_leds; i++) {
        h->buf[i].color      = color;
        h->buf[i].brightness = brightness;
    }
}

void ws2812_clear(ws2812_handle_t h)
{
    if (!h) return;
    memset(h->buf, 0, h->num_leds * sizeof(ws2812_pixel_t));
    led_strip_clear(h->strip);
}

void ws2812_refresh(ws2812_handle_t h)
{
    if (h) _push(h);
}

// ─────────────────────────────────────────────────────────────
//  UTILIDADES DE COLOR
// ─────────────────────────────────────────────────────────────
ws2812_color_t ws2812_hsv(uint16_t hue, uint8_t sat, uint8_t val)
{
    hue %= 360;
    uint8_t hi = hue / 60;
    uint8_t f  = (hue % 60) * 255 / 60;
    uint8_t p  = _scale(val, 255 - sat);
    uint8_t q  = _scale(val, 255 - _scale(f, sat));
    uint8_t t  = _scale(val, 255 - _scale(255 - f, sat));
    switch (hi) {
        case 0:  return (ws2812_color_t){val, t,   p  };
        case 1:  return (ws2812_color_t){q,   val, p  };
        case 2:  return (ws2812_color_t){p,   val, t  };
        case 3:  return (ws2812_color_t){p,   q,   val};
        case 4:  return (ws2812_color_t){t,   p,   val};
        default: return (ws2812_color_t){val, p,   q  };
    }
}

ws2812_color_t ws2812_blend(ws2812_color_t a, ws2812_color_t b, uint8_t t)
{
    return (ws2812_color_t){
        .r = a.r + (int16_t)(b.r - a.r) * t / 255,
        .g = a.g + (int16_t)(b.g - a.g) * t / 255,
        .b = a.b + (int16_t)(b.b - a.b) * t / 255,
    };
}

// ─────────────────────────────────────────────────────────────
//  EFECTOS
// ─────────────────────────────────────────────────────────────
void ws2812_fx_chase(ws2812_handle_t h, ws2812_color_t color,
                     uint8_t brightness, uint32_t delay_ms, uint8_t repeat)
{
    if (!h) return;
    for (int r = 0; r < repeat; r++) {
        for (uint16_t i = 0; i < h->num_leds; i++) {
            ws2812_clear(h);
            ws2812_set_pixel(h, i, color, brightness);
            _push(h);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
    ws2812_clear(h);
}

void ws2812_fx_wipe(ws2812_handle_t h, ws2812_color_t color,
                    uint8_t brightness, uint32_t delay_ms)
{
    if (!h) return;
    for (uint16_t i = 0; i < h->num_leds; i++) {
        ws2812_set_pixel(h, i, color, brightness);
        _push(h);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

void ws2812_fx_rainbow(ws2812_handle_t h, uint8_t brightness,
                       uint32_t delay_ms, uint8_t repeat)
{
    if (!h) return;
    for (int r = 0; r < repeat; r++) {
        for (int offset = 0; offset < 360; offset += 5) {
            for (uint16_t i = 0; i < h->num_leds; i++) {
                uint16_t hue = (offset + i * 360 / h->num_leds) % 360;
                ws2812_set_pixel(h, i, ws2812_hsv(hue, 255, 255), brightness);
            }
            _push(h);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
    ws2812_clear(h);
}

void ws2812_fx_breathe(ws2812_handle_t h, ws2812_color_t color,
                       uint32_t cycle_ms, uint8_t repeat)
{
    if (!h) return;
    uint16_t steps   = 60;
    uint32_t step_ms = cycle_ms / (steps * 2);

    for (int r = 0; r < repeat; r++) {
        for (int i = 0; i <= steps; i++) {
            uint8_t b = i * 255 / steps;
            ws2812_fill(h, color, b);
            _push(h);
            vTaskDelay(pdMS_TO_TICKS(step_ms));
        }
        for (int i = steps; i >= 0; i--) {
            uint8_t b = i * 255 / steps;
            ws2812_fill(h, color, b);
            _push(h);
            vTaskDelay(pdMS_TO_TICKS(step_ms));
        }
    }
    ws2812_clear(h);
}

void ws2812_fx_sparkle(ws2812_handle_t h, ws2812_color_t color,
                       uint8_t brightness, uint32_t delay_ms, uint16_t total_flashes)
{
    if (!h) return;
    for (int f = 0; f < total_flashes; f++) {
        ws2812_clear(h);
        uint8_t idx = esp_random() % h->num_leds;
        ws2812_set_pixel(h, idx, color, brightness);
        _push(h);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    ws2812_clear(h);
}

void ws2812_fx_fire(ws2812_handle_t h, uint32_t duration_ms)
{
    if (!h) return;
    uint32_t end = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms);
    while (xTaskGetTickCount() < end) {
        for (uint16_t i = 0; i < h->num_leds; i++) {
            uint8_t flicker = esp_random() % 100;
            ws2812_color_t fire = {200 + flicker, 20 + (flicker / 5), 0};
            ws2812_set_pixel(h, i, fire, 200 + flicker / 2);
        }
        _push(h);
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    ws2812_clear(h);
}

void ws2812_fx_comet(ws2812_handle_t h, ws2812_color_t color,
                     uint8_t brightness, uint8_t tail_len,
                     uint32_t delay_ms, uint8_t repeat)
{
    if (!h) return;
    int total = h->num_leds + tail_len;
    for (int r = 0; r < repeat; r++) {
        for (int pos = 0; pos < total; pos++) {
            ws2812_clear(h);
            for (int t = 0; t <= tail_len; t++) {
                int idx = pos - t;
                if (idx >= 0 && idx < h->num_leds) {
                    uint8_t fade = 255 - (t * 255 / (tail_len + 1));
                    uint8_t b    = _scale(brightness, fade);
                    ws2812_set_pixel(h, idx, color, b);
                }
            }
            _push(h);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
    ws2812_clear(h);
}

void ws2812_fx_bounce(ws2812_handle_t h, ws2812_color_t color,
                      uint8_t brightness, uint32_t delay_ms, uint8_t repeat)
{
    if (!h) return;
    for (int r = 0; r < repeat; r++) {
        for (int i = 0; i < h->num_leds; i++) {
            ws2812_clear(h);
            ws2812_set_pixel(h, i, color, brightness);
            _push(h);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
        for (int i = h->num_leds - 2; i > 0; i--) {
            ws2812_clear(h);
            ws2812_set_pixel(h, i, color, brightness);
            _push(h);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
    ws2812_clear(h);
}

void ws2812_fx_theater(ws2812_handle_t h, ws2812_color_t color,
                       uint8_t brightness, uint32_t delay_ms, uint8_t repeat)
{
    if (!h) return;
    for (int r = 0; r < repeat; r++) {
        for (int phase = 0; phase < 3; phase++) {
            ws2812_clear(h);
            for (uint16_t i = phase; i < h->num_leds; i += 3) {
                ws2812_set_pixel(h, i, color, brightness);
            }
            _push(h);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
    ws2812_clear(h);
}

void ws2812_fx_fade(ws2812_handle_t h, ws2812_color_t from, ws2812_color_t to,
                    uint8_t brightness, uint32_t duration_ms)
{
    if (!h) return;
    uint16_t steps   = 60;
    uint32_t step_ms = duration_ms / steps;
    for (int i = 0; i <= steps; i++) {
        ws2812_fill(h, ws2812_blend(from, to, i * 255 / steps), brightness);
        _push(h);
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }
}

void ws2812_fx_blink(ws2812_handle_t h, ws2812_color_t color,
                     uint8_t brightness, uint32_t on_ms,
                     uint32_t off_ms, uint8_t repeat)
{
    if (!h) return;
    for (int r = 0; r < repeat; r++) {
        ws2812_fill(h, color, brightness);
        _push(h);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        ws2812_clear(h);
        vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
}