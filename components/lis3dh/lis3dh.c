#include "lis3dh.h"
#include "driver/i2c.h"
#include "esp_log.h"

static const char *TAG = "lis3dh";
static uint8_t g_dev_addr = 0x00;
static int g_i2c_port = I2C_NUM_0;

void lis3dh_set_i2c_port(int i2c_port) {
    g_i2c_port = i2c_port;
}

esp_err_t lis3dh_i2c_write_reg(uint8_t addr7, uint8_t reg, const uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) return ESP_ERR_NO_MEM;

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr7 << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    if (len && data) {
        i2c_master_write(cmd, (uint8_t*)data, len, true);
    }
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(g_i2c_port, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    return ret;
}

esp_err_t lis3dh_i2c_read_reg(uint8_t addr7, uint8_t reg, uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) return ESP_ERR_NO_MEM;

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr7 << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);  // El reg ya debe incluir auto-incremento
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr7 << 1) | I2C_MASTER_READ, true);

    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, &data[len - 1], I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(g_i2c_port, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    return ret;
}

bool lis3dh_detect(uint8_t *addr_found)
{
    const uint8_t cand[2] = {LIS3DH_ADDR_SA0_LOW, LIS3DH_ADDR_SA0_HIGH};
    uint8_t who = 0;

    for (int i = 0; i < 2; ++i) {
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (lis3dh_i2c_read_reg(cand[i], LIS3DH_REG_WHO_AM_I, &who, 1) == ESP_OK) {
                ESP_LOGI(TAG, "Addr 0x%02X -> WHO_AM_I=0x%02X", cand[i], who);
                if (who == LIS3DH_WHO_AM_I_VAL) {
                    g_dev_addr = cand[i];
                    if (addr_found) *addr_found = cand[i];
                    return true;
                }
            } else {
                ESP_LOGW(TAG, "I2C read failed at addr 0x%02X (attempt %d)", cand[i], attempt + 1);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
    }
    ESP_LOGE(TAG, "No LIS3DH detected after multiple attempts");
    return false;
}

esp_err_t lis3dh_init_default(void)
{
    if (g_dev_addr == 0x00) return ESP_ERR_INVALID_STATE;

    // CTRL1: 100Hz, normal mode, enable all axes
    uint8_t ctrl1 = 0x57;
    esp_err_t ret = lis3dh_i2c_write_reg(g_dev_addr, LIS3DH_REG_CTRL1, &ctrl1, 1);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(10));

    // CTRL2: Filtros desactivados
    uint8_t ctrl2 = 0x00;
    ret = lis3dh_i2c_write_reg(g_dev_addr, 0x21, &ctrl2, 1);
    if (ret != ESP_OK) return ret;

    // CTRL3: Interrupciones desactivadas
    uint8_t ctrl3 = 0x00;
    ret = lis3dh_i2c_write_reg(g_dev_addr, 0x22, &ctrl3, 1);
    if (ret != ESP_OK) return ret;

    // CTRL4: ±2g + High Resolution + BDU
    uint8_t ctrl4 = 0x88;
    ret = lis3dh_i2c_write_reg(g_dev_addr, LIS3DH_REG_CTRL4, &ctrl4, 1);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(10));

    // CTRL5: Interrupciones desactivadas
    uint8_t ctrl5 = 0x00;
    ret = lis3dh_i2c_write_reg(g_dev_addr, LIS3DH_REG_CTRL5, &ctrl5, 1);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "LIS3DH initialized successfully");
    return ESP_OK;
}

float lis3dh_sensitivity_g_per_lsb(void)
{
    // Para LIS3DH en modo High Resolution (±2g) → 16384 LSB/g
    // Sensibilidad = 1/16384 = 0.000061035 g/LSB
    return 0.000061035f;  // 61 µg/LSB
}

esp_err_t lis3dh_read_xyz_g(float *xg, float *yg, float *zg)
{
    if (g_dev_addr == 0x00) return ESP_ERR_INVALID_STATE;

    uint8_t raw[6] = {0};
    
    // IMPORTANTE: Para lectura múltiple, se debe usar el bit de auto-incremento
    // El registro OUT_X_L (0x28) con auto-incremento es 0x80 | 0x28 = 0xA8
    uint8_t reg_auto_inc = 0xA8;  // 0x80 (auto-increment) + 0x28 (OUT_X_L)
    
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) return ESP_ERR_NO_MEM;

    // Escribir dirección del registro con auto-incremento
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (g_dev_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg_auto_inc, true);
    
    // Leer 6 bytes (X_L, X_H, Y_L, Y_H, Z_L, Z_H)
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (g_dev_addr << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, raw, 5, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, &raw[5], I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(g_i2c_port, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    
    if (ret != ESP_OK) return ret;

    // Convertir a valores con signo
    int16_t x = (int16_t)((raw[1] << 8) | raw[0]);
    int16_t y = (int16_t)((raw[3] << 8) | raw[2]);
    int16_t z = (int16_t)((raw[5] << 8) | raw[4]);

    // Depuración
    static int contador = 0;
    contador++;
    if (contador % 10 == 0) {
        ESP_LOGI(TAG, "RAW: X=%d Y=%d Z=%d", x, y, z);
    }

    float g_per_lsb = lis3dh_sensitivity_g_per_lsb();
    if (xg) *xg = (float)x * g_per_lsb;
    if (yg) *yg = (float)y * g_per_lsb;
    if (zg) *zg = (float)z * g_per_lsb;
    return ESP_OK;
}

esp_err_t lis3dh_calibrate_bias_g(int samples, int delay_ms,
                                   float *bias_xg, float *bias_yg, float *bias_zg)
{
    if (samples <= 0) return ESP_ERR_INVALID_ARG;
    float sx = 0.f, sy = 0.f, sz = 0.f;
    int valid_samples = 0;

    for (int i = 0; i < samples; ++i) {
        float xg, yg, zg;
        esp_err_t ret = lis3dh_read_xyz_g(&xg, &yg, &zg);
        if (ret == ESP_OK) {
            sx += xg;
            sy += yg;
            sz += zg;
            valid_samples++;
        }
        if (delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    if (valid_samples == 0) return ESP_FAIL;

    // Bias = promedio - gravedad esperada
    // En reposo: X=0, Y=0, Z=1g
    if (bias_xg) *bias_xg = sx / valid_samples;
    if (bias_yg) *bias_yg = sy / valid_samples;
    if (bias_zg) *bias_zg = (sz / valid_samples) - 1.0f;  // Restar 1g en Z
    
    ESP_LOGI(TAG, "Calibración: X=%.4f Y=%.4f Z=%.4f (muestras=%d)", 
             sx/valid_samples, sy/valid_samples, sz/valid_samples, valid_samples);
    
    return ESP_OK;
}