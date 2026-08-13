#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "led_strip.h"

// ─────────────────────────────────────────────────────────────
//  HANDLE
// ─────────────────────────────────────────────────────────────
typedef struct ws2812_t *ws2812_handle_t;

// ─────────────────────────────────────────────────────────────
//  COLOR
// ─────────────────────────────────────────────────────────────
typedef struct {
    uint8_t r, g, b;
} ws2812_color_t;

#define COLOR_RED        (ws2812_color_t){255, 0,   0  }
#define COLOR_GREEN      (ws2812_color_t){0,   255, 0  }
#define COLOR_BLUE       (ws2812_color_t){0,   0,   255}
#define COLOR_WHITE      (ws2812_color_t){255, 255, 255}
#define COLOR_YELLOW     (ws2812_color_t){255, 255, 0  }
#define COLOR_CYAN       (ws2812_color_t){0,   255, 255}
#define COLOR_MAGENTA    (ws2812_color_t){255, 0,   255}
#define COLOR_ORANGE     (ws2812_color_t){255, 80,  0  }
#define COLOR_PURPLE     (ws2812_color_t){80,  0,   255}
#define COLOR_OFF        (ws2812_color_t){0,   0,   0  }

// ─────────────────────────────────────────────────────────────
//  PIXEL INTERNO  (color + brillo propios)
// ─────────────────────────────────────────────────────────────
typedef struct {
    ws2812_color_t color;
    uint8_t        brightness;  // 0–255 por LED individual
} ws2812_pixel_t;

// ─────────────────────────────────────────────────────────────
//  CONFIG
// ─────────────────────────────────────────────────────────────
typedef struct {
    gpio_num_t gpio;
    uint16_t   num_leds;
} ws2812_config_t;

#define WS2812_DEFAULT_CONFIG() { \
    .gpio     = GPIO_NUM_4,       \
    .num_leds = 5,                \
}

// ─────────────────────────────────────────────────────────────
//  INIT / DEINIT
// ─────────────────────────────────────────────────────────────
esp_err_t ws2812_init(const ws2812_config_t *cfg, ws2812_handle_t *out);
void      ws2812_deinit(ws2812_handle_t h);

// ─────────────────────────────────────────────────────────────
//  CONTROL BÁSICO
// ─────────────────────────────────────────────────────────────

/** Enciende un LED con su color y brillo propios. */
void ws2812_set_pixel(ws2812_handle_t h, uint16_t index,
                      ws2812_color_t color, uint8_t brightness);

/** Pone todos los LEDs con el mismo color y brillo. */
void ws2812_fill(ws2812_handle_t h, ws2812_color_t color, uint8_t brightness);

/** Apaga todos los LEDs. */
void ws2812_clear(ws2812_handle_t h);

/** Envía el buffer a los LEDs. Siempre llamar al final. */
void ws2812_refresh(ws2812_handle_t h);

// ─────────────────────────────────────────────────────────────
//  UTILIDADES DE COLOR
// ─────────────────────────────────────────────────────────────

/** Convierte HSV → RGB.  h:0–360  s:0–255  v:0–255 */
ws2812_color_t ws2812_hsv(uint16_t hue, uint8_t sat, uint8_t val);

/** Mezcla dos colores.  t: 0=a  255=b */
ws2812_color_t ws2812_blend(ws2812_color_t a, ws2812_color_t b, uint8_t t);

// ─────────────────────────────────────────────────────────────
//  EFECTOS  (brightness propio para cada efecto)
// ─────────────────────────────────────────────────────────────
void ws2812_fx_chase(ws2812_handle_t h, ws2812_color_t color,
                     uint8_t brightness, uint32_t delay_ms, uint8_t repeat);

void ws2812_fx_wipe(ws2812_handle_t h, ws2812_color_t color,
                    uint8_t brightness, uint32_t delay_ms);

void ws2812_fx_rainbow(ws2812_handle_t h, uint8_t brightness,
                       uint32_t delay_ms, uint8_t repeat);

void ws2812_fx_breathe(ws2812_handle_t h, ws2812_color_t color,
                       uint32_t cycle_ms, uint8_t repeat);

void ws2812_fx_sparkle(ws2812_handle_t h, ws2812_color_t color,
                       uint8_t brightness, uint32_t delay_ms, uint16_t total_flashes);

void ws2812_fx_fire(ws2812_handle_t h, uint32_t duration_ms);

void ws2812_fx_comet(ws2812_handle_t h, ws2812_color_t color,
                     uint8_t brightness, uint8_t tail_len,
                     uint32_t delay_ms, uint8_t repeat);

void ws2812_fx_bounce(ws2812_handle_t h, ws2812_color_t color,
                      uint8_t brightness, uint32_t delay_ms, uint8_t repeat);

void ws2812_fx_theater(ws2812_handle_t h, ws2812_color_t color,
                       uint8_t brightness, uint32_t delay_ms, uint8_t repeat);

void ws2812_fx_fade(ws2812_handle_t h, ws2812_color_t from, ws2812_color_t to,
                    uint8_t brightness, uint32_t duration_ms);

void ws2812_fx_blink(ws2812_handle_t h, ws2812_color_t color,
                     uint8_t brightness, uint32_t on_ms,
                     uint32_t off_ms, uint8_t repeat);