#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "lis3dh.h"
#include "ws2812.h"
#include "dy_sv17f.h"

#define TAG "SISTEMA_CINTURONES"

// ==========================================
//    CONFIGURACIÓN GENERAL Y PINES
// ==========================================
#define PIN_CONTACTO        GPIO_NUM_41
#define PIN_PULSADOR        GPIO_NUM_11
#define PIN_RELAY           GPIO_NUM_1 

#define PIN_PILOTO          GPIO_NUM_10 
#define PIN_COPILOTO        GPIO_NUM_9 
#define PIN_PASAJERO_IZQ    GPIO_NUM_46 
#define PIN_PASAJERO_MED    GPIO_NUM_3 
#define PIN_PASAJERO_DER    GPIO_NUM_8 

#define PIN_LED_WS2812      GPIO_NUM_13
#define PIN_DY_TX           GPIO_NUM_17 // Pin TX exclusivo para DY-SV17F

#define I2C_PORT            I2C_NUM_0
#define I2C_SDA             GPIO_NUM_48
#define I2C_SCL             GPIO_NUM_47
#define I2C_FREQ            50000     

#define G_STD                   9.80665f
#define WINDOW_SIZE             10

// ==========================================
//    VARIABLES CONFIGURABLES DEL SISTEMA
// ==========================================
static int g_cantidadAsientosConfig = 2;
static int g_numLeds = 5;
static float g_desvioUmbral = 0.50f;
static int g_toleranciaMovimientoSeg = 5;

// Configuración individual de Lógica de Lectura por cinturón:
// 1 = Cinturón abrochado a 3.3V (Lógica normal)
// 0 = Cinturón abrochado a 0V   (Lógica invertida)
static int g_nivelAbrochadoPiloto      = 1;
static int g_nivelAbrochadoCopiloto     = 0;
static int g_nivelAbrochadoPasajeroIzq = 1;
static int g_nivelAbrochadoPasajeroMed = 1;
static int g_nivelAbrochadoPasajeroDer = 1;

typedef struct {
    const char* nombre;
    gpio_num_t pin;
    int ledIndex;
    bool habilitado;
    int nivelAbrochado; // Guarda la lógica configurada para este asiento
} asiento_t;

static asiento_t asientos[5] = {
    { "Piloto",            PIN_PILOTO,       0, false, 1 }, // Audio 7
    { "Copiloto",           PIN_COPILOTO,      1, false, 1 }, // Audio 6
    { "Pasajero Izquierdo", PIN_PASAJERO_IZQ,  2, false, 1 }, // Audio 9
    { "Pasajero Medio",     PIN_PASAJERO_MED,  3, false, 1 }, // Audio 13 o 14 (si son 3 asientos)
    { "Pasajero Derecho",   PIN_PASAJERO_DER,  4, false, 1 }  // Audio 8
};

static int numAsientosHabilitados = 0;

typedef struct {
    bool sistemaActivo;
    bool relayDesactivado;
    int pasajerosDeclarados;
    int totalAbrochados;
    bool abrochados[5];
    bool anteriores[5];
    bool alertaAsiento[5];

    // Variables individuales de infracción por asiento (no arreglo),
    // pensadas para poder enviarse una por una (MQTT/JSON) más adelante.
    // Se ponen en 1 en el instante en que suena el audio de alerta del asiento
    // y vuelven a 0 apenas ese asiento se abrocha nuevamente.
    bool infraccionPiloto;
    bool infraccionCopiloto;
    bool infraccionPasajeroIzq;
    bool infraccionPasajeroMed;
    bool infraccionPasajeroDer;

    bool carroEnMovimiento;
    bool movimientoConfirmado;
    float aceleracionActual;
    float desvioActual;
} estado_sistema_t;

static estado_sistema_t g_estado = {0};
static SemaphoreHandle_t g_mutex = NULL;
static dy_sv17f_handle_t g_dy_handle = NULL;

static float bxG = 0, byG = 0, bzG = 0;
static float acelBuffer[WINDOW_SIZE];
static int indiceBuffer = 0;
static bool bufferLleno = false;
static bool sensorDisponible = false;

// ==========================================
// FUNCIONES AUXILIARES Y HARDWARE
// ==========================================
static void reproducirAudio(uint16_t index) {
    if (!g_dy_handle) return;
    dy_stop(g_dy_handle);
    vTaskDelay(pdMS_TO_TICKS(20));
    dy_play_index(g_dy_handle, index);
}

static bool esperarOInterrumpir(uint32_t duracion_ms, uint16_t track) {
    uint32_t transcurrido = 0;
    while (transcurrido < duracion_ms) {
        vTaskDelay(pdMS_TO_TICKS(50));
        transcurrido += 50;

        xSemaphoreTake(g_mutex, portMAX_DELAY);
        estado_sistema_t st = g_estado;
        xSemaphoreGive(g_mutex);

        if (!st.sistemaActivo) return true;

        if (track == 6 || track == 7 || track == 8 || track == 9 || track == 10 || track == 13 || track == 14) {
            if (!st.carroEnMovimiento && !st.movimientoConfirmado) return true;

            int alertas_activas = 0;
            for (int i = 0; i < 5; i++) {
                if (asientos[i].habilitado && st.alertaAsiento[i]) alertas_activas++;
            }

            if (alertas_activas == 0) return true;
        }
    }
    return false;
}

static void configurarAsientos(int cantidad) {
    // Asignar los niveles lógicos individuales configurados globalmente
    asientos[0].nivelAbrochado = g_nivelAbrochadoPiloto;
    asientos[1].nivelAbrochado = g_nivelAbrochadoCopiloto;
    asientos[2].nivelAbrochado = g_nivelAbrochadoPasajeroIzq;
    asientos[3].nivelAbrochado = g_nivelAbrochadoPasajeroMed;
    asientos[4].nivelAbrochado = g_nivelAbrochadoPasajeroDer;

    for (int i = 0; i < 5; i++) asientos[i].habilitado = false;
    switch (cantidad) {
        case 1: asientos[0].habilitado = true; break;
        case 2: asientos[0].habilitado = true; asientos[1].habilitado = true; break;
        case 3: asientos[0].habilitado = true; asientos[1].habilitado = true; asientos[3].habilitado = true; break;
        case 4: asientos[0].habilitado = true; asientos[1].habilitado = true; asientos[2].habilitado = true; asientos[4].habilitado = true; break;
        case 5: default: for (int i = 0; i < 5; i++) asientos[i].habilitado = true; break;
    }
    numAsientosHabilitados = 0;
    for (int i = 0; i < 5; i++) {
        if (asientos[i].habilitado) numAsientosHabilitados++;
    }
}

static void configurarPines(void) {
    gpio_config_t ioConfContacto = { .pin_bit_mask = (1ULL << PIN_CONTACTO), .mode = GPIO_MODE_INPUT, .pull_down_en = GPIO_PULLDOWN_ENABLE }; 
    gpio_config(&ioConfContacto);
    gpio_wakeup_enable(PIN_CONTACTO, GPIO_INTR_HIGH_LEVEL);
    
    gpio_config_t ioConfPulsador = { .pin_bit_mask = (1ULL << PIN_PULSADOR), .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE }; 
    gpio_config(&ioConfPulsador);
    
    uint64_t maskCinturones = 0;
    for (int i = 0; i < 5; i++) {
        if (asientos[i].habilitado) maskCinturones |= (1ULL << asientos[i].pin);
    }
    
    if (maskCinturones > 0) {
        gpio_config_t ioConfCinturones = { .pin_bit_mask = maskCinturones, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE }; 
        gpio_config(&ioConfCinturones);
    }
    
    gpio_config_t ioConfSalida = { .pin_bit_mask = (1ULL << PIN_RELAY), .mode = GPIO_MODE_OUTPUT }; 
    gpio_config(&ioConfSalida);
    gpio_set_level(PIN_RELAY, 0); 
}

static void i2cMasterInit(void) {
    i2c_config_t conf = { 
        .mode = I2C_MODE_MASTER, .sda_io_num = I2C_SDA, .scl_io_num = I2C_SCL, 
        .sda_pullup_en = GPIO_PULLUP_ENABLE, .scl_pullup_en = GPIO_PULLUP_ENABLE, .master.clk_speed = I2C_FREQ 
    };
    i2c_param_config(I2C_PORT, &conf); 
    i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0);
}

bool detectarMovimiento(float nueva, float *desvio_out) {
    acelBuffer[indiceBuffer] = nueva; 
    indiceBuffer++;
    if (indiceBuffer >= WINDOW_SIZE) { indiceBuffer = 0; bufferLleno = true; }
    if (!bufferLleno) return false;
    
    float media = 0; 
    for (int i = 0; i < WINDOW_SIZE; i++) media += acelBuffer[i]; 
    media /= WINDOW_SIZE;
    
    float var = 0; 
    for (int i = 0; i < WINDOW_SIZE; i++) { float d = acelBuffer[i] - media; var += d * d; } 
    var /= WINDOW_SIZE;
    float desvio = sqrtf(var);
    if (desvio_out) *desvio_out = desvio;
    
    return (desvio > g_desvioUmbral);
}

// ==========================================
// TAREA: ACELERÓMETRO E I2C
// ==========================================
void task_acelerometro(void *pvParameters) {
    i2cMasterInit(); 
    lis3dh_set_i2c_port(I2C_PORT); 
    uint8_t addr;
    if (lis3dh_detect(&addr)) { 
        if (lis3dh_init_default() == ESP_OK) { 
            sensorDisponible = true; 
            vTaskDelay(pdMS_TO_TICKS(150)); 
            lis3dh_calibrate_bias_g(10, 10, &bxG, &byG, &bzG); 
            ESP_LOGI(TAG, "Sensor LIS3DH inicializado y calibrado.");
        } 
    } else {
        ESP_LOGE(TAG, "No se detectó el sensor LIS3DH.");
    }

    int temporizadorMovimiento = 0;
    bool movimiento_previo = false;
    int contador_tolerancia = 0;

    while (1) {
        float aceleracion = 0.0f;
        float desvio = 0.0f;

        if (sensorDisponible) {
            float xg, yg, zg;
            if (lis3dh_read_xyz_g(&xg, &yg, &zg) == ESP_OK) {
                float cx = (xg - bxG) * G_STD, cy = (yg - byG) * G_STD, cz = (zg - bzG) * G_STD;
                aceleracion = sqrtf(cx * cx + cy * cy + cz * cz);
                if (detectarMovimiento(aceleracion, &desvio)) temporizadorMovimiento = 40;
            }
        }

        bool movActual = (temporizadorMovimiento > 0);
        if (temporizadorMovimiento > 0) temporizadorMovimiento--;

        // Cálculo dinámico de ciclos requeridos (50 ms por cada iteración del bucle)
        int ciclosToleranciaRequeridos = (g_toleranciaMovimientoSeg * 1000) / 50;

        bool movConfirmado = false;
        if (movActual) {
            if (!movimiento_previo) {
                contador_tolerancia = 0;
            } else {
                contador_tolerancia++;
                if (contador_tolerancia >= ciclosToleranciaRequeridos) {
                    movConfirmado = true;
                }
            }
        } else {
            contador_tolerancia = 0;
        }
        movimiento_previo = movActual;

        xSemaphoreTake(g_mutex, portMAX_DELAY);
        g_estado.aceleracionActual = aceleracion;
        g_estado.desvioActual = desvio;
        g_estado.carroEnMovimiento = movConfirmado; // Pasa a 1 tras cumplir los segundos de tolerancia
        g_estado.movimientoConfirmado = movConfirmado;
        xSemaphoreGive(g_mutex);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ==========================================
// TAREA: PULSADOR
// ==========================================
void task_pulsador(void *pvParameters) {
    int ultimoEstadoPulsador = 1;

    while (1) {
        int lectura = gpio_get_level(PIN_PULSADOR);

        if (lectura == 0 && ultimoEstadoPulsador == 1) { 
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            if (g_estado.sistemaActivo) {
                if (!g_estado.relayDesactivado) {
                    g_estado.pasajerosDeclarados++;
                    if (g_estado.pasajerosDeclarados > numAsientosHabilitados) g_estado.pasajerosDeclarados = 1;
                    ESP_LOGI(TAG, "Selección manual: %d tripulantes", g_estado.pasajerosDeclarados);
                } else {
                    if (g_estado.movimientoConfirmado || g_estado.carroEnMovimiento) {
                        ESP_LOGW(TAG, "🚫 Ajuste DENEGADO: Vehículo en movimiento.");
                    } else {
                        // Validar específicamente que el CHOFER (asiento 0) esté abrochado
                        if (g_estado.abrochados[0]) {
                            g_estado.pasajerosDeclarados = g_estado.totalAbrochados;
                            for (int i = 0; i < 5; i++) {
                                if (!g_estado.abrochados[i]) g_estado.alertaAsiento[i] = false;
                            }
                            ESP_LOGI(TAG, "Pasajeros reajustados correctamente: %d", g_estado.pasajerosDeclarados);
                        } else {
                            g_estado.alertaAsiento[0] = true;
                            ESP_LOGW(TAG, "Ajuste denegado: El chofer debe tener el cinturón abrochado.");
                        }
                    }
                }
            }
            xSemaphoreGive(g_mutex);
            vTaskDelay(pdMS_TO_TICKS(200)); 
        }
        ultimoEstadoPulsador = lectura;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ==========================================
// TAREA: LÓGICA DE CINTURONES Y GPIOs
// ==========================================
void task_cinturones_logica(void *pvParameters) {
    configurarAsientos(g_cantidadAsientosConfig);
    configurarPines();

    int ciclosContactoBajo = 0;
    int temporizadorApagado = 0;
    bool apagadoPendiente = false;

    while (1) {
        bool contactoFisico = (gpio_get_level(PIN_CONTACTO) == 1);

        xSemaphoreTake(g_mutex, portMAX_DELAY);

        if (contactoFisico) {
            ciclosContactoBajo = 0;
            apagadoPendiente = false;
            if (!g_estado.sistemaActivo) {
                g_estado.sistemaActivo = true;
                g_estado.relayDesactivado = false;
                g_estado.pasajerosDeclarados = 0;
                gpio_set_level(PIN_RELAY, 1);

                // Reset de infracciones individuales al iniciar un nuevo trayecto
                g_estado.infraccionPiloto = false;
                g_estado.infraccionCopiloto = false;
                g_estado.infraccionPasajeroIzq = false;
                g_estado.infraccionPasajeroMed = false;
                g_estado.infraccionPasajeroDer = false;

                for (int i = 0; i < 5; i++) {
                    g_estado.alertaAsiento[i] = false;
                    // Lógica individual por asiento para determinar si estaba abrochado
                    if (asientos[i].habilitado) {
                        g_estado.anteriores[i] = (gpio_get_level(asientos[i].pin) == asientos[i].nivelAbrochado);
                    }
                }
                ESP_LOGI(TAG, "Contacto ON -> Relé ACTIVADO (1).");
            }
        } else {
            if (g_estado.sistemaActivo) {
                ciclosContactoBajo++;
                if (ciclosContactoBajo >= 200) { 
                    g_estado.sistemaActivo = false;
                    g_estado.relayDesactivado = false;
                    gpio_set_level(PIN_RELAY, 0);
                    for (int i = 0; i < 5; i++) g_estado.alertaAsiento[i] = false;
                    apagadoPendiente = true;
                    temporizadorApagado = 100; 
                    ESP_LOGI(TAG, "Contacto OFF -> Relé DESACTIVADO (0V)");
                }
            }
        }

        if (g_estado.sistemaActivo) {
            g_estado.totalAbrochados = 0;
            for (int i = 0; i < 5; i++) {
                if (asientos[i].habilitado) {
                    // Lógica individual por asiento para evaluar el pin físico
                    g_estado.abrochados[i] = (gpio_get_level(asientos[i].pin) == asientos[i].nivelAbrochado);
                    if (g_estado.abrochados[i]) g_estado.totalAbrochados++;
                }
            }

            if (g_estado.relayDesactivado) {
                for (int i = 0; i < 5; i++) {
                    if (asientos[i].habilitado) {
                        bool recien = (!g_estado.anteriores[i] && g_estado.abrochados[i]);
                        if (recien && (g_estado.totalAbrochados > g_estado.pasajerosDeclarados) && (g_estado.totalAbrochados <= numAsientosHabilitados)) {
                            g_estado.pasajerosDeclarados = g_estado.totalAbrochados;
                            ESP_LOGI(TAG, "Pasajeros auto-actualizados: %d", g_estado.pasajerosDeclarados);
                            break;
                        }
                    }
                }
            }

            for (int i = 0; i < 5; i++) {
                if (!asientos[i].habilitado) continue;
                if (g_estado.abrochados[i]) {
                    g_estado.alertaAsiento[i] = false;

                    // Reset inmediato de la infracción al abrocharse nuevamente
                    switch (i) {
                        case 0: g_estado.infraccionPiloto = false; break;
                        case 1: g_estado.infraccionCopiloto = false; break;
                        case 2: g_estado.infraccionPasajeroIzq = false; break;
                        case 3: g_estado.infraccionPasajeroMed = false; break;
                        case 4: g_estado.infraccionPasajeroDer = false; break;
                    }
                } else if (g_estado.anteriores[i] && !g_estado.abrochados[i] && g_estado.relayDesactivado) {
                    g_estado.alertaAsiento[i] = true;
                }
                g_estado.anteriores[i] = g_estado.abrochados[i];
            }

            if (!g_estado.relayDesactivado) {
                bool pilotoOk = g_estado.abrochados[0];
                bool seleccionHecha = (g_estado.pasajerosDeclarados > 0);
                bool cantidadOk = (g_estado.pasajerosDeclarados == g_estado.totalAbrochados);

                if (seleccionHecha && pilotoOk && cantidadOk) {
                    gpio_set_level(PIN_RELAY, 0);
                    g_estado.relayDesactivado = true;
                    ESP_LOGI(TAG, "✅ Condiciones cumplidas -> Relé DESACTIVADO (0V)");
                }
            }
        }

        xSemaphoreGive(g_mutex);

        if (apagadoPendiente) {
            temporizadorApagado--;
            if (temporizadorApagado <= 0) {
                apagadoPendiente = false;
                vTaskDelay(pdMS_TO_TICKS(50));
                gpio_wakeup_enable(PIN_CONTACTO, GPIO_INTR_HIGH_LEVEL);
                esp_sleep_enable_gpio_wakeup();
                esp_light_sleep_start();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ==========================================
// TAREA: CONTROL DE REPRODUCCIÓN DE AUDIOS
// ==========================================
void task_audio(void *pvParameters) {
    dy_sv17f_config_t dy_cfg = {
        .uart_num = UART_NUM_1,
        .baud_rate = 9600,
        .tx_pin = PIN_DY_TX,
        .rx_pin = GPIO_NUM_NC,
        .busy_pin = GPIO_NUM_NC,
        .rx_buf_size = 256,
        .tx_buf_size = 256,
        .on_done = NULL,
        .on_done_arg = NULL
    };

    if (dy_sv17f_init(&dy_cfg, &g_dy_handle) == ESP_OK) {
        dy_set_volume(g_dy_handle, 20);
        ESP_LOGI(TAG, "Audio DY-SV17F inicializado a Volumen 20.");
    } else {
        ESP_LOGE(TAG, "Error inicializando módulo de Audio DY-SV17F.");
    }

    bool last_sistemaActivo = false;
    bool last_relayDesactivado = false;
    int last_pasajerosDeclarados = 0;

    while (1) {
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        estado_sistema_t st = g_estado;
        xSemaphoreGive(g_mutex);

        // 1. Audio 11: Contacto pasa a 1
        if (!last_sistemaActivo && st.sistemaActivo) {
            reproducirAudio(11);
            last_sistemaActivo = true;
            last_relayDesactivado = false;
            last_pasajerosDeclarados = 0;
            esperarOInterrumpir(1000, 11);
            continue;
        }

        if (!st.sistemaActivo) {
            last_sistemaActivo = false;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // 2. Audios 1 al 5: Cambio en Pasajeros Declarados
        if (!st.relayDesactivado && st.pasajerosDeclarados != last_pasajerosDeclarados) {
            if (st.pasajerosDeclarados >= 1 && st.pasajerosDeclarados <= numAsientosHabilitados) {
                reproducirAudio(st.pasajerosDeclarados);
            }
            last_pasajerosDeclarados = st.pasajerosDeclarados;
            esperarOInterrumpir(1000, st.pasajerosDeclarados);
            continue;
        }

        // 3. Audio 12: Desactivación del relé
        if (!last_relayDesactivado && st.relayDesactivado) {
            reproducirAudio(12);
            last_relayDesactivado = true;
            esperarOInterrumpir(1000, 12);
            continue;
        }

        // 4. Audios de Alerta en Movimiento (6, 7, 8, 9, 10, 13, 14)
        if (st.carroEnMovimiento || st.movimientoConfirmado) {
            int alertas_activas = 0;
            int ultimo_asiento_alerta = -1;

            for (int i = 0; i < 5; i++) {
                if (asientos[i].habilitado && st.alertaAsiento[i]) {
                    alertas_activas++;
                    ultimo_asiento_alerta = i;
                }
            }

            if (alertas_activas > 0) {
                uint16_t track = 0;
                uint32_t duracion_play_ms = 0;

                if (alertas_activas > 1) {
                    track = 10;
                    duracion_play_ms = 10000;
                } else {
                    switch (ultimo_asiento_alerta) {
                        case 0: track = 7;  duracion_play_ms = 6000; break; // Piloto
                        case 1: track = 6;  duracion_play_ms = 6000; break; // Copiloto
                        case 2: track = 9;  duracion_play_ms = 9000; break; // Pasajero Izq (+1s)
                        case 3: // Pasajero Medio
                            if (numAsientosHabilitados == 3) {
                                track = 14;          // Audio 14 solo cuando son 3 asientos
                                duracion_play_ms = 6000; // 6 segundos de reproducción
                            } else {
                                track = 13;          // Audio 13 para otras configuraciones
                                duracion_play_ms = 8000;
                            }
                            break;
                        case 4: track = 8;  duracion_play_ms = 9000; break; // Pasajero Der (+1s)
                        default: break;
                    }
                }

                if (track > 0) {
                    // Marcar infracción del/los asiento(s) que están sonando en este instante.
                    xSemaphoreTake(g_mutex, portMAX_DELAY);
                    for (int i = 0; i < 5; i++) {
                        if (asientos[i].habilitado && st.alertaAsiento[i]) {
                            switch (i) {
                                case 0: g_estado.infraccionPiloto = true; break;
                                case 1: g_estado.infraccionCopiloto = true; break;
                                case 2: g_estado.infraccionPasajeroIzq = true; break;
                                case 3: g_estado.infraccionPasajeroMed = true; break;
                                case 4: g_estado.infraccionPasajeroDer = true; break;
                            }
                        }
                    }
                    xSemaphoreGive(g_mutex);

                    reproducirAudio(track);
                    
                    // Esperar la duración del audio o interrumpir si se abrochan
                    bool interrumpido = esperarOInterrumpir(duracion_play_ms, track);
                    dy_stop(g_dy_handle);

                    // Si no fue interrumpido, esperar 2 segundos de pausa previa a repetir
                    if (!interrumpido) {
                        esperarOInterrumpir(2000, track);
                    }
                    continue;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ==========================================
// TAREA: CONTROL DE LEDS WS2812
// ==========================================
void task_leds(void *pvParameters) {
    ws2812_config_t ws_cfg = { .gpio = PIN_LED_WS2812, .num_leds = g_numLeds };
    ws2812_handle_t leds = NULL;

    if (ws2812_init(&ws_cfg, &leds) != ESP_OK) {
        ESP_LOGE(TAG, "Error al inicializar WS2812");
        vTaskDelete(NULL);
    }

    const ws2812_color_t COLOR_VERDE   = {0, 255, 0};
    const ws2812_color_t COLOR_ROJO    = {255, 0, 0};
    const ws2812_color_t COLOR_APAGADO = {0, 0, 0};
    const uint8_t BRILLO = 200;

    while (1) {
        xSemaphoreTake(g_mutex, portMAX_DELAY);

        if (!g_estado.sistemaActivo) {
            ws2812_clear(leds);
        } else {
            for (int i = 0; i < 5; i++) {
                int ledIdx = asientos[i].ledIndex;
                if (ledIdx >= g_numLeds) continue;

                if (!asientos[i].habilitado) {
                    ws2812_set_pixel(leds, ledIdx, COLOR_APAGADO, 0);
                    continue;
                }

                if (g_estado.abrochados[i]) {
                    ws2812_set_pixel(leds, ledIdx, COLOR_VERDE, BRILLO);
                } else if (g_estado.alertaAsiento[i]) {
                    ws2812_set_pixel(leds, ledIdx, COLOR_ROJO, BRILLO);
                } else {
                    ws2812_set_pixel(leds, ledIdx, COLOR_APAGADO, 0);
                }
            }
            ws2812_refresh(leds);
        }

        xSemaphoreGive(g_mutex);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ==========================================
// TAREA: TELEMETRÍA Y LOGS
// ==========================================
void task_telemetria(void *pvParameters) {
    while (1) {
        xSemaphoreTake(g_mutex, portMAX_DELAY);

        if (g_estado.sistemaActivo) {
            bool algunaAlerta = false;
            for (int i = 0; i < 5; i++) {
                if (g_estado.alertaAsiento[i]) algunaAlerta = true;
            }

            char strCinturones[192] = "";
            for (int i = 0; i < 5; i++) {
                if (!asientos[i].habilitado) continue;

                bool inf = false;
                switch (i) {
                    case 0: inf = g_estado.infraccionPiloto; break;
                    case 1: inf = g_estado.infraccionCopiloto; break;
                    case 2: inf = g_estado.infraccionPasajeroIzq; break;
                    case 3: inf = g_estado.infraccionPasajeroMed; break;
                    case 4: inf = g_estado.infraccionPasajeroDer; break;
                }

                char tmp[48];
                snprintf(tmp, sizeof(tmp), "[%s:%d|Inf:%d] ",
                         asientos[i].nombre,
                         g_estado.abrochados[i] ? 1 : 0,
                         inf ? 1 : 0);
                strcat(strCinturones, tmp);
            }

            printf("Cont:%d | Decl:%d | Abrochados:%d | Acc:%.2f | Desv:%.2f | Relé:%d | Mov:%d | Alerta:%d | %s\n",
                g_estado.sistemaActivo,
                g_estado.pasajerosDeclarados,
                g_estado.totalAbrochados,
                g_estado.aceleracionActual,
                g_estado.desvioActual,
                gpio_get_level(PIN_RELAY),
                g_estado.movimientoConfirmado,
                algunaAlerta,
                strCinturones);
        }

        xSemaphoreGive(g_mutex);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ==========================================
// PUNTO DE ENTRADA PRINCIPAL
// ==========================================
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_log_level_set("lis3dh", ESP_LOG_WARN);
    g_mutex = xSemaphoreCreateMutex();

    // Tareas FreeRTOS
    xTaskCreatePinnedToCore(task_pulsador,        "Task_Pulsador",   3072, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(task_cinturones_logica, "Task_Cinturones", 4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(task_leds,             "Task_Leds",       3072, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(task_audio,            "Task_Audio",      4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(task_acelerometro,     "Task_Acel",       3072, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(task_telemetria,       "Task_Telemetria", 3072, NULL, 1, NULL, 1);

    ESP_LOGI(TAG, "Sistema de Cinturones con Audio Lanzado Con Éxito.");
}