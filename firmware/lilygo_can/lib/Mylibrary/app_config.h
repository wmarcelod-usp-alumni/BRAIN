#pragma once
#include <Arduino.h>

// Configurações centralizadas do projeto
namespace AppConfig {

// --- CAN / OBD ---
static constexpr uint32_t CAN_BITRATE = 500000;     // ISO 15765-4 (11-bit / 500k)
static constexpr bool     CAN_LISTEN_ONLY = false;  // precisa ser FALSE para enviar requisições OBD/UDS

// Alguns transceivers (como no T-CAN485) usam um pino para modo/slope control.
// 0 = alta velocidade (menos slope control), 1 = baixa/standby (depende do circuito).
static constexpr uint8_t  CAN_SPEED_MODE_LEVEL = LOW;

// --- Wi‑Fi AP / Portal ---
static constexpr const char* WIFI_SSID = "CAN-LOGGER";
static constexpr const char* WIFI_PASS = "12345678"; // mínimo 8 chars (ESP32 softAP)
static constexpr uint16_t    WIFI_PORT = 80;

// --- Logging / Arquivos ---
static constexpr const char* LOG_DIR = "/logs";
static constexpr uint32_t    LOG_FLUSH_MS = 1000;

// --- Telemetria / debug ---
static constexpr uint32_t    STATS_PRINT_MS = 1000;

// Logs no Serial podem impactar performance em tráfego CAN alto.
// Para coletas longas (dataset), recomenda-se deixar as opções VERBOSE abaixo em false.
static constexpr bool SERIAL_VERBOSE_OBD = false;   // imprime valores OBD decodificados
static constexpr bool SERIAL_VERBOSE_UDS = false;   // imprime payloads/erros UDS
static constexpr bool SERIAL_VERBOSE_PID_BITMAP = false; // imprime bitmaps de PIDs suportados

// --- OBD polling (Mode 01) ---
static constexpr uint32_t    OBD_REQ_INTERVAL_MS = 200;  // 1 request a cada 200 ms (ajuste conforme necessário)
static constexpr uint32_t    OBD_RESP_TIMEOUT_MS = 150;  // timeout por request (ms)

// --- UDS polling opcional (service 0x22) ---
// Par observado no seu canlog_004.csv (req 0x78A / resp 0x7CA) com DID 0xD001.
// Você pode desabilitar no app_config.h ou no código (ObdPoller::setUdsEnabled()).
static constexpr bool        UDS_DEFAULT_ENABLED = true;
static constexpr uint32_t    UDS_REQ_INTERVAL_MS = 250;

} // namespace AppConfig
