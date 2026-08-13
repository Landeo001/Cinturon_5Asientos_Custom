#ifndef LIS3DH_H
#define LIS3DH_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// ============================================================
//  DIRECCIONES I2C PARA LIS3DH
// ============================================================
#define LIS3DH_ADDR_SA0_LOW   0x18   // SA0 = 0 (tierra)
#define LIS3DH_ADDR_SA0_HIGH  0x19   // SA0 = 1 (VCC)

// ============================================================
//  REGISTROS DEL LIS3DH
// ============================================================
#define LIS3DH_REG_WHO_AM_I   0x0F
#define LIS3DH_WHO_AM_I_VAL   0x33   // ← Diferente: 0x33 vs 0x3F

#define LIS3DH_REG_CTRL1      0x20   // Control 1 (ODR, enable axes)
#define LIS3DH_REG_CTRL2      0x21   // Control 2 (filtros)
#define LIS3DH_REG_CTRL3      0x22   // Control 3 (interrupciones)
#define LIS3DH_REG_CTRL4      0x23   // Control 4 (rango, BDU, etc.)
#define LIS3DH_REG_CTRL5      0x24   // Control 5 (interrupciones)
#define LIS3DH_REG_CTRL6      0x25   // Control 6 (interrupciones)

#define LIS3DH_REG_OUT_X_L    0x28
#define LIS3DH_REG_OUT_X_H    0x29
#define LIS3DH_REG_OUT_Y_L    0x2A
#define LIS3DH_REG_OUT_Y_H    0x2B
#define LIS3DH_REG_OUT_Z_L    0x2C
#define LIS3DH_REG_OUT_Z_H    0x2D

// ============================================================
//  CONFIGURACIÓN DE RANGO
// ============================================================
// CTRL4 bits
#define LIS3DH_CTRL4_BDU      0x80   // Block Data Update
#define LIS3DH_CTRL4_FS_2G    0x00   // ±2g
#define LIS3DH_CTRL4_FS_4G    0x10   // ±4g
#define LIS3DH_CTRL4_FS_8G    0x20   // ±8g
#define LIS3DH_CTRL4_FS_16G   0x30   // ±16g
#define LIS3DH_CTRL4_HR       0x08   // High Resolution

// CTRL1 bits (ODR y enable)
#define LIS3DH_CTRL1_ODR_1Hz   0x00
#define LIS3DH_CTRL1_ODR_10Hz  0x10
#define LIS3DH_CTRL1_ODR_25Hz  0x20
#define LIS3DH_CTRL1_ODR_50Hz  0x30
#define LIS3DH_CTRL1_ODR_100Hz 0x40   // ← Usamos este
#define LIS3DH_CTRL1_ODR_200Hz 0x50
#define LIS3DH_CTRL1_ODR_400Hz 0x60
#define LIS3DH_CTRL1_ODR_1kHz  0x70
#define LIS3DH_CTRL1_ODR_5kHz  0x80
#define LIS3DH_CTRL1_Xen       0x01
#define LIS3DH_CTRL1_Yen       0x02
#define LIS3DH_CTRL1_Zen       0x04

// ============================================================
//  SENSIBILIDADES (mg/LSB)
// ============================================================
#define LIS3DH_SENS_2G_mg    0.061f    // 1 mg/LSB
#define LIS3DH_SENS_4G_mg    0.122f
#define LIS3DH_SENS_8G_mg    0.244f
#define LIS3DH_SENS_16G_mg   0.732f

// Sensibilidad en g/LSB
#define LIS3DH_SENS_2G_g     0.000061f
#define LIS3DH_SENS_4G_g     0.000122f
#define LIS3DH_SENS_8G_g     0.000244f
#define LIS3DH_SENS_16G_g    0.000732f

#ifdef __cplusplus
extern "C" {
#endif

void lis3dh_set_i2c_port(int i2c_port);

esp_err_t lis3dh_i2c_write_reg(uint8_t addr7, uint8_t reg, const uint8_t *data, size_t len);
esp_err_t lis3dh_i2c_read_reg(uint8_t addr7, uint8_t reg, uint8_t *data, size_t len);

bool lis3dh_detect(uint8_t *addr_found);
esp_err_t lis3dh_init_default(void);
float lis3dh_sensitivity_g_per_lsb(void);
esp_err_t lis3dh_read_xyz_g(float *xg, float *yg, float *zg);
esp_err_t lis3dh_calibrate_bias_g(int samples, int delay_ms,
                                   float *bias_xg, float *bias_yg, float *bias_zg);

#ifdef __cplusplus
}
#endif

#endif