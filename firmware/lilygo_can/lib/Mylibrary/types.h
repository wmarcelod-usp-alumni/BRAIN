#pragma once
#include <Arduino.h>

struct CanFrame {
  uint64_t ts_us = 0;
  uint32_t id    = 0;
  bool     ext   = false;
  bool     rtr   = false;
  uint8_t  dlc   = 0;
  uint8_t  data[8] = {0};
};

struct CanStatus {
  int      state = 0;                 // TWAI_STATE_*
  uint32_t bus_error_count = 0;
  uint32_t rx_missed_count = 0;
  uint32_t rx_overrun_count = 0;
// Contadores de erro do controlador CAN.
// Para diagnóstico rápido:
// - tx_error_counter subindo: transmissões sem ACK (outro nó não está ACKando, ou barramento desconectado)
// - rx_error_counter subindo: problemas de recepção (bitrate incorreto, terminação, ruído, CANH/CANL invertido)
uint32_t tx_error_counter = 0;
uint32_t rx_error_counter = 0;
uint32_t tx_failed_count  = 0;
uint32_t arb_lost_count   = 0;
uint32_t msgs_to_tx       = 0;
uint32_t msgs_to_rx       = 0;

};

struct RuntimeStats {
  // Contadores (atualizados no loop principal)
  uint32_t rx_frames = 0;
  uint32_t tx_frames = 0;

  // FPS calculado a cada 1s
  uint32_t fps = 0;

  // Último status do controlador CAN
  CanStatus can;

  // Estado do SD
  bool sd_ok = false;
};

inline const char* twaiStateToStr(int st) {
  switch (st) {
    case 0: return "STOPPED";
    case 1: return "RUNNING";
    case 2: return "BUS_OFF";
    case 3: return "RECOVERY";
    default: return "UNKNOWN";
  }
}
