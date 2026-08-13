#include "dy_sv17f.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>   // <-- AGREGAR para PRIu32

static const char *TAG = "DY_SV17F";

// ─────────────────────────────────────────────
//  STRUCT INTERNO
// ─────────────────────────────────────────────
struct dy_sv17f_t {
    dy_sv17f_config_t  cfg;
    SemaphoreHandle_t  tx_mutex;
    SemaphoreHandle_t  done_sem;
    TaskHandle_t       cb_task;
};

// ─────────────────────────────────────────────
//  ISR: se ejecuta cuando BUSY sube (audio terminó)
// ─────────────────────────────────────────────
static void IRAM_ATTR _busy_isr_handler(void *arg) {
    struct dy_sv17f_t *dev = (struct dy_sv17f_t *)arg;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(dev->done_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

// ─────────────────────────────────────────────
//  TASK: ejecuta el callback on_done fuera de ISR
// ─────────────────────────────────────────────
static void _cb_task(void *arg) {
    struct dy_sv17f_t *dev = (struct dy_sv17f_t *)arg;
    while (1) {
        if (xSemaphoreTake(dev->done_sem, portMAX_DELAY) == pdTRUE) {
            int lvl = (dev->cfg.busy_pin != GPIO_NUM_NC)
                      ? gpio_get_level(dev->cfg.busy_pin) : -1;
            ESP_LOGI(TAG, "done_sem tomado — BUSY pin nivel: %d", lvl);
            if (dev->cfg.on_done) {
                dev->cfg.on_done(dev, dev->cfg.on_done_arg);
            }
        }
    }
}

// ─────────────────────────────────────────────
//  CHECKSUM: low 8 bits de la suma de todos los bytes
// ─────────────────────────────────────────────
static uint8_t _cs(uint8_t cmd, const uint8_t *data, uint8_t len) {
    uint8_t s = DY_FRAME_HEAD + cmd + len;
    for (uint8_t i = 0; i < len; i++) s += data[i];
    return s;
}

// ─────────────────────────────────────────────
//  INIT
// ─────────────────────────────────────────────
esp_err_t dy_sv17f_init(const dy_sv17f_config_t *cfg, dy_sv17f_handle_t *out_handle) {
    if (!cfg || !out_handle) return ESP_ERR_INVALID_ARG;

    struct dy_sv17f_t *dev = calloc(1, sizeof(struct dy_sv17f_t));
    if (!dev) return ESP_ERR_NO_MEM;
    dev->cfg = *cfg;

    dev->tx_mutex = xSemaphoreCreateMutex();
    if (!dev->tx_mutex) { free(dev); return ESP_ERR_NO_MEM; }

    dev->done_sem = xSemaphoreCreateBinary();
    if (!dev->done_sem) {
        vSemaphoreDelete(dev->tx_mutex);
        free(dev);
        return ESP_ERR_NO_MEM;
    }

    const uart_config_t uart_cfg = {
        .baud_rate  = cfg->baud_rate,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    #if SOC_UART_SUPPORT_XTAL_CLK
        .source_clk = UART_SCLK_XTAL,
    #else
        .source_clk = UART_SCLK_APB,
    #endif
    };
    
    esp_err_t r;
    r = uart_driver_install(cfg->uart_num, cfg->rx_buf_size, cfg->tx_buf_size, 0, NULL, 0);
    if (r != ESP_OK) goto fail;
    r = uart_param_config(cfg->uart_num, &uart_cfg);
    if (r != ESP_OK) goto fail;
    r = uart_set_pin(cfg->uart_num, cfg->tx_pin, cfg->rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (r != ESP_OK) goto fail;
    uart_set_line_inverse(cfg->uart_num, UART_SIGNAL_INV_DISABLE);

    if (cfg->busy_pin != GPIO_NUM_NC) {
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << cfg->busy_pin),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type    = GPIO_INTR_NEGEDGE,
        };
        r = gpio_config(&io);
        if (r != ESP_OK) goto fail;

        gpio_install_isr_service(0);
        r = gpio_isr_handler_add(cfg->busy_pin, _busy_isr_handler, dev);
        if (r != ESP_OK) goto fail;

        ESP_LOGI(TAG, "BUSY ISR en GPIO%d", cfg->busy_pin);

        if (cfg->on_done) {
            xTaskCreate(_cb_task, "dy_cb", 2048, dev, 5, &dev->cb_task);
        }
    }

    *out_handle = dev;
    // CORREGIDO: %lubaud → %" PRIu32 "baud
    ESP_LOGI(TAG, "DY-SV17F OK — UART%d TX:GPIO%d RX:GPIO%d %" PRIu32 "baud",
             cfg->uart_num, cfg->tx_pin, cfg->rx_pin, cfg->baud_rate);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "init falló: %s", esp_err_to_name(r));
    vSemaphoreDelete(dev->tx_mutex);
    vSemaphoreDelete(dev->done_sem);
    free(dev);
    return r;
}

// ─────────────────────────────────────────────
//  DEINIT
// ─────────────────────────────────────────────
void dy_sv17f_deinit(dy_sv17f_handle_t handle) {
    if (!handle) return;
    if (handle->cfg.busy_pin != GPIO_NUM_NC) {
        gpio_isr_handler_remove(handle->cfg.busy_pin);
    }
    if (handle->cb_task) vTaskDelete(handle->cb_task);
    uart_driver_delete(handle->cfg.uart_num);
    vSemaphoreDelete(handle->tx_mutex);
    vSemaphoreDelete(handle->done_sem);
    free(handle);
    ESP_LOGI(TAG, "deinit OK");
}

// ─────────────────────────────────────────────
//  BAJO NIVEL — thread-safe con mutex
// ─────────────────────────────────────────────
void dy_send_cmd(dy_sv17f_handle_t h, uint8_t cmd, const uint8_t *data, uint8_t len) {
    if (!h) return;
    uint8_t frame[16];
    uint8_t idx = 0;
    frame[idx++] = DY_FRAME_HEAD;
    frame[idx++] = cmd;
    frame[idx++] = len;
    for (uint8_t i = 0; i < len; i++) frame[idx++] = data[i];
    frame[idx++] = _cs(cmd, data, len);

    xSemaphoreTake(h->tx_mutex, portMAX_DELAY);
    uart_write_bytes(h->cfg.uart_num, (const char *)frame, idx);
    xSemaphoreGive(h->tx_mutex);

    ESP_LOGD(TAG, "TX 0x%02X (%d bytes)", cmd, idx);
}

// CORREGIDO: %ums → %" PRIu32 "ms
bool dy_read_response(dy_sv17f_handle_t h, dy_response_t *resp, uint32_t timeout_ms) {
    if (!h || !resp) return false;
    memset(resp, 0, sizeof(*resp));
    uart_flush_input(h->cfg.uart_num);
    int n = uart_read_bytes(h->cfg.uart_num, resp->raw, sizeof(resp->raw), pdMS_TO_TICKS(timeout_ms));
    if (n <= 0) { 
        ESP_LOGW(TAG, "Sin respuesta (%" PRIu32 "ms)", timeout_ms); 
        return false; 
    }
    resp->len = (uint8_t)n;
    if (n >= 4) {
        uint8_t cs = 0;
        for (int i = 0; i < n - 1; i++) cs += resp->raw[i];
        resp->valid = (cs == resp->raw[n - 1]);
        if (!resp->valid) ESP_LOGW(TAG, "Checksum inválido");
        if      (n >= 5) resp->value = ((uint16_t)resp->raw[3] << 8) | resp->raw[4];
        else if (n >= 4) resp->value = resp->raw[3];
    }
    return resp->valid;
}

// ─────────────────────────────────────────────
//  BUSY — ISR-driven, CPU = 0 mientras espera
// ─────────────────────────────────────────────
bool dy_is_busy(dy_sv17f_handle_t h) {
    if (!h || h->cfg.busy_pin == GPIO_NUM_NC) return false;
    return gpio_get_level(h->cfg.busy_pin) == 1;
}

// CORREGIDO: %ums → %" PRIu32 "ms
void dy_wait_done(dy_sv17f_handle_t h, uint32_t timeout_ms) {
    if (!h || h->cfg.busy_pin == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "dy_wait_done: busy_pin no configurado");
        return;
    }
    TickType_t ticks = timeout_ms ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;
    if (xSemaphoreTake(h->done_sem, ticks) == pdFALSE) {
        ESP_LOGW(TAG, "dy_wait_done: timeout (%" PRIu32 "ms)", timeout_ms);
    }
}

// ─────────────────────────────────────────────
//  REPRODUCCIÓN
// ─────────────────────────────────────────────
void dy_play(dy_sv17f_handle_t h)         { dy_send_cmd(h, DY_CMD_PLAY,         NULL, 0); }
void dy_pause(dy_sv17f_handle_t h)        { dy_send_cmd(h, DY_CMD_PAUSE,        NULL, 0); }
void dy_stop(dy_sv17f_handle_t h)         { dy_send_cmd(h, DY_CMD_STOP,         NULL, 0); }
void dy_stop_playing(dy_sv17f_handle_t h) { dy_send_cmd(h, DY_CMD_STOP_PLAYING, NULL, 0); }
void dy_prev(dy_sv17f_handle_t h)         { dy_send_cmd(h, DY_CMD_PREV,         NULL, 0); }
void dy_next(dy_sv17f_handle_t h)         { dy_send_cmd(h, DY_CMD_NEXT,         NULL, 0); }
void dy_prev_file(dy_sv17f_handle_t h)    { dy_send_cmd(h, DY_CMD_PREV_FILE,    NULL, 0); }
void dy_next_file(dy_sv17f_handle_t h)    { dy_send_cmd(h, DY_CMD_NEXT_FILE,    NULL, 0); }

void dy_play_index(dy_sv17f_handle_t h, uint16_t index) {
    uint8_t d[2] = { (index >> 8) & 0xFF, index & 0xFF };
    dy_send_cmd(h, DY_CMD_PLAY_INDEX, d, 2);
    ESP_LOGI(TAG, "PLAY #%u", index);
}

void dy_select_no_play(dy_sv17f_handle_t h, uint16_t index) {
    uint8_t d[2] = { (index >> 8) & 0xFF, index & 0xFF };
    dy_send_cmd(h, DY_CMD_SELECT_NO_PLAY, d, 2);
}

void dy_interplay(dy_sv17f_handle_t h, dy_drive_t drive, uint16_t index) {
    uint8_t d[3] = { (uint8_t)drive, (index >> 8) & 0xFF, index & 0xFF };
    dy_send_cmd(h, DY_CMD_INTERPLAY, d, 3);
    ESP_LOGI(TAG, "INTERPLAY drive=%u #%u", drive, index);
}

// ─────────────────────────────────────────────
//  VOLUMEN
// ─────────────────────────────────────────────
void dy_set_volume(dy_sv17f_handle_t h, uint8_t vol) {
    if (vol > 30) vol = 30;
    uint8_t d[1] = { vol };
    dy_send_cmd(h, DY_CMD_SET_VOLUME, d, 1);
    ESP_LOGI(TAG, "VOL %u/30", vol);
}

void dy_volume_up(dy_sv17f_handle_t h)   { dy_send_cmd(h, DY_CMD_VOL_UP,   NULL, 0); }
void dy_volume_down(dy_sv17f_handle_t h) { dy_send_cmd(h, DY_CMD_VOL_DOWN, NULL, 0); }

// ─────────────────────────────────────────────
//  CONFIGURACIÓN
// ─────────────────────────────────────────────
void dy_set_loop(dy_sv17f_handle_t h, dy_loop_mode_t m) {
    uint8_t d[1] = { (uint8_t)m }; dy_send_cmd(h, DY_CMD_SET_LOOP, d, 1);
}

void dy_set_eq(dy_sv17f_handle_t h, dy_eq_t eq) {
    uint8_t d[1] = { (uint8_t)eq }; dy_send_cmd(h, DY_CMD_SET_EQ, d, 1);
}

void dy_switch_drive(dy_sv17f_handle_t h, dy_drive_t drv) {
    uint8_t d[1] = { (uint8_t)drv }; dy_send_cmd(h, DY_CMD_SWITCH_DRIVE, d, 1);
}

void dy_set_cycle_times(dy_sv17f_handle_t h, uint16_t t) {
    uint8_t d[2] = { (t >> 8) & 0xFF, t & 0xFF }; 
    dy_send_cmd(h, DY_CMD_SET_CYCLE_TIMES, d, 2);
}

// ─────────────────────────────────────────────
//  CONSULTAS
// ─────────────────────────────────────────────
dy_state_t dy_query_state(dy_sv17f_handle_t h) {
    dy_send_cmd(h, DY_CMD_QUERY_STATUS, NULL, 0);
    dy_response_t r; 
    if (dy_read_response(h, &r, 500) && r.len >= 4) return (dy_state_t)r.raw[3];
    return DY_STATE_STOP;
}

uint16_t dy_query_total_tracks(dy_sv17f_handle_t h) {
    dy_send_cmd(h, DY_CMD_QUERY_TOTAL, NULL, 0);
    dy_response_t r; 
    return dy_read_response(h, &r, 500) ? r.value : 0;
}

uint16_t dy_query_current_track(dy_sv17f_handle_t h) {
    dy_send_cmd(h, DY_CMD_QUERY_CURRENT, NULL, 0);
    dy_response_t r; 
    return dy_read_response(h, &r, 500) ? r.value : 0;
}

dy_drive_t dy_query_online_drive(dy_sv17f_handle_t h) {
    dy_send_cmd(h, DY_CMD_QUERY_ONLINE_DRV, NULL, 0);
    dy_response_t r; 
    if (dy_read_response(h, &r, 500) && r.len >= 4) return (dy_drive_t)r.raw[3];
    return DY_DRIVE_NO_DEVICE;
}

dy_drive_t dy_query_play_drive(dy_sv17f_handle_t h) {
    dy_send_cmd(h, DY_CMD_QUERY_PLAY_DRV, NULL, 0);
    dy_response_t r; 
    if (dy_read_response(h, &r, 500) && r.len >= 4) return (dy_drive_t)r.raw[3];
    return DY_DRIVE_NO_DEVICE;
}

uint16_t dy_query_folder_song(dy_sv17f_handle_t h) {
    dy_send_cmd(h, DY_CMD_QUERY_FOLDER_SNG, NULL, 0);
    dy_response_t r; 
    return dy_read_response(h, &r, 500) ? r.value : 0;
}

uint16_t dy_query_folder_song_count(dy_sv17f_handle_t h) {
    dy_send_cmd(h, DY_CMD_QUERY_FOLDER_NUM, NULL, 0);
    dy_response_t r; 
    return dy_read_response(h, &r, 500) ? r.value : 0;
}