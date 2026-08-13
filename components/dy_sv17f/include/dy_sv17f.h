/**
 * @file dy_sv17f.h
 * @brief Driver DY-SV17F — producción
 *
 * Mejoras vs versión anterior:
 *  - ISR en BUSY pin (rising edge) → semáforo FreeRTOS. CPU = 0 mientras espera.
 *  - Mutex en UART → thread-safe desde múltiples tasks.
 *  - Callback on_done → programación orientada a eventos, sin bloquear.
 *
 * Uso básico (bloqueante):
 *   dy_sv17f_config_t cfg = DY_SV17F_DEFAULT_CONFIG();
 *   cfg.uart_num = UART_NUM_1;
 *   cfg.tx_pin   = GPIO_NUM_20;
 *   cfg.rx_pin   = GPIO_NUM_18;
 *   cfg.busy_pin = GPIO_NUM_5;
 *
 *   dy_sv17f_handle_t audio = NULL;
 *   ESP_ERROR_CHECK(dy_sv17f_init(&cfg, &audio));
 *
 *   dy_set_volume(audio, 25);
 *   dy_play_index(audio, 1);
 *   dy_wait_done(audio, 10000);   // duerme el task, CPU libre
 *
 * Uso con callback (no bloqueante):
 *   static void on_audio_done(dy_sv17f_handle_t h, void *arg) {
 *       ESP_LOGI("APP", "Audio terminó");
 *   }
 *   cfg.on_done     = on_audio_done;
 *   cfg.on_done_arg = NULL;
 */

#ifndef DY_SV17F_H
#define DY_SV17F_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_err.h"

// ─────────────────────────────────────────────
//  FORWARD DECLARATIONS
// ─────────────────────────────────────────────
typedef struct dy_sv17f_t *dy_sv17f_handle_t;

/**
 * @brief Callback llamado cuando el audio termina (BUSY HIGH)
 *        Se ejecuta en contexto de task, no en ISR — es seguro hacer logs, etc.
 * @param handle  Handle del dispositivo que terminó
 * @param arg     Argumento de usuario registrado en on_done_arg
 */
typedef void (*dy_done_cb_t)(dy_sv17f_handle_t handle, void *arg);

// ─────────────────────────────────────────────
//  CONFIGURACIÓN
// ─────────────────────────────────────────────
typedef struct {
    uart_port_t  uart_num;     ///< UART_NUM_0 / 1 / 2
    gpio_num_t   tx_pin;       ///< GPIO TX  → RXD módulo
    gpio_num_t   rx_pin;       ///< GPIO RX  ← TXD módulo
    gpio_num_t   busy_pin;     ///< GPIO     ← BUSY módulo (pin 12). GPIO_NUM_NC = no usar
    uint32_t     baud_rate;    ///< 9600 (fijo en DY-SV17F)
    int          rx_buf_size;
    int          tx_buf_size;
    dy_done_cb_t on_done;      ///< Callback al terminar el audio. NULL = no usar
    void        *on_done_arg;  ///< Argumento libre para el callback
} dy_sv17f_config_t;

#define DY_SV17F_DEFAULT_CONFIG() {  \
    .uart_num    = UART_NUM_1,       \
    .tx_pin      = GPIO_NUM_20,      \
    .rx_pin      = GPIO_NUM_18,      \
    .busy_pin    = GPIO_NUM_NC,      \
    .baud_rate   = 9600,             \
    .rx_buf_size = 256,              \
    .tx_buf_size = 256,              \
    .on_done     = NULL,             \
    .on_done_arg = NULL,             \
}

// ─────────────────────────────────────────────
//  COMANDOS (datasheet pág 5-6)
// ─────────────────────────────────────────────
#define DY_FRAME_HEAD           0xAA
#define DY_CMD_PLAY             0x02
#define DY_CMD_PAUSE            0x03
#define DY_CMD_STOP             0x04
#define DY_CMD_PREV             0x05
#define DY_CMD_NEXT             0x06
#define DY_CMD_PLAY_INDEX       0x07
#define DY_CMD_PREV_FILE        0x0E
#define DY_CMD_NEXT_FILE        0x0F
#define DY_CMD_STOP_PLAYING     0x10
#define DY_CMD_VOL_UP           0x14
#define DY_CMD_VOL_DOWN         0x15
#define DY_CMD_INTERPLAY        0x16
#define DY_CMD_SET_LOOP         0x18
#define DY_CMD_SET_CYCLE_TIMES  0x19
#define DY_CMD_SET_EQ           0x1A
#define DY_CMD_SELECT_NO_PLAY   0x1F
#define DY_CMD_SWITCH_DRIVE     0x0B
#define DY_CMD_SET_VOLUME       0x13
#define DY_CMD_QUERY_STATUS     0x01
#define DY_CMD_QUERY_ONLINE_DRV 0x09
#define DY_CMD_QUERY_PLAY_DRV   0x0A
#define DY_CMD_QUERY_TOTAL      0x0C
#define DY_CMD_QUERY_CURRENT    0x0D
#define DY_CMD_QUERY_FOLDER_SNG 0x11
#define DY_CMD_QUERY_FOLDER_NUM 0x12

// ─────────────────────────────────────────────
//  ENUMS
// ─────────────────────────────────────────────
typedef enum {
    DY_LOOP_ALL_CYCLE    = 0x00,
    DY_LOOP_SINGLE_CYCLE = 0x01,
    DY_LOOP_SINGLE_STOP  = 0x02,  ///< DEFAULT al encender
    DY_LOOP_RANDOM       = 0x03,
    DY_LOOP_DIR_CYCLE    = 0x04,
    DY_LOOP_DIR_RANDOM   = 0x05,
    DY_LOOP_DIR_ORDER    = 0x06,
    DY_LOOP_SEQ_STOP     = 0x07,
} dy_loop_mode_t;

typedef enum {
    DY_EQ_NORMAL  = 0x00,
    DY_EQ_POP     = 0x01,
    DY_EQ_ROCK    = 0x02,
    DY_EQ_JAZZ    = 0x03,
    DY_EQ_CLASSIC = 0x04,
} dy_eq_t;

typedef enum {
    DY_DRIVE_USB       = 0x00,
    DY_DRIVE_SD        = 0x01,
    DY_DRIVE_FLASH     = 0x02,
    DY_DRIVE_NO_DEVICE = 0xFF,
} dy_drive_t;

typedef enum {
    DY_STATE_STOP  = 0x00,
    DY_STATE_PLAY  = 0x01,
    DY_STATE_PAUSE = 0x02,
} dy_state_t;

typedef struct {
    uint8_t  raw[16];
    uint8_t  len;
    uint16_t value;
    bool     valid;
} dy_response_t;

// ─────────────────────────────────────────────
//  API PÚBLICA
// ─────────────────────────────────────────────

esp_err_t dy_sv17f_init(const dy_sv17f_config_t *cfg, dy_sv17f_handle_t *out_handle);
void      dy_sv17f_deinit(dy_sv17f_handle_t handle);

// ── Reproducción ─────────────────────────────
void dy_play(dy_sv17f_handle_t handle);
void dy_pause(dy_sv17f_handle_t handle);
void dy_stop(dy_sv17f_handle_t handle);
void dy_stop_playing(dy_sv17f_handle_t handle);
void dy_prev(dy_sv17f_handle_t handle);
void dy_next(dy_sv17f_handle_t handle);
void dy_prev_file(dy_sv17f_handle_t handle);
void dy_next_file(dy_sv17f_handle_t handle);
void dy_play_index(dy_sv17f_handle_t handle, uint16_t index);
void dy_select_no_play(dy_sv17f_handle_t handle, uint16_t index);
void dy_interplay(dy_sv17f_handle_t handle, dy_drive_t drive, uint16_t index);

// ── Volumen ──────────────────────────────────
void dy_set_volume(dy_sv17f_handle_t handle, uint8_t vol);
void dy_volume_up(dy_sv17f_handle_t handle);
void dy_volume_down(dy_sv17f_handle_t handle);

// ── Configuración ────────────────────────────
void dy_set_loop(dy_sv17f_handle_t handle, dy_loop_mode_t mode);
void dy_set_eq(dy_sv17f_handle_t handle, dy_eq_t eq);
void dy_switch_drive(dy_sv17f_handle_t handle, dy_drive_t drive);
void dy_set_cycle_times(dy_sv17f_handle_t handle, uint16_t times);

// ── Consultas ────────────────────────────────
dy_state_t dy_query_state(dy_sv17f_handle_t handle);
uint16_t   dy_query_total_tracks(dy_sv17f_handle_t handle);
uint16_t   dy_query_current_track(dy_sv17f_handle_t handle);
dy_drive_t dy_query_online_drive(dy_sv17f_handle_t handle);
dy_drive_t dy_query_play_drive(dy_sv17f_handle_t handle);
uint16_t   dy_query_folder_song(dy_sv17f_handle_t handle);
uint16_t   dy_query_folder_song_count(dy_sv17f_handle_t handle);

// ── BUSY ─────────────────────────────────────
bool dy_is_busy(dy_sv17f_handle_t handle);

/**
 * @brief Bloquea el task actual hasta que el audio termine.
 *        Usa semáforo FreeRTOS + ISR — CPU = 0 mientras espera.
 *        Si busy_pin = GPIO_NUM_NC, retorna inmediatamente con warning.
 * @param timeout_ms  0 = esperar indefinidamente
 */
void dy_wait_done(dy_sv17f_handle_t handle, uint32_t timeout_ms);

// ── Bajo nivel ───────────────────────────────
void dy_send_cmd(dy_sv17f_handle_t handle, uint8_t cmd, const uint8_t *data, uint8_t len);
bool dy_read_response(dy_sv17f_handle_t handle, dy_response_t *resp, uint32_t timeout_ms);

#endif // DY_SV17F_H