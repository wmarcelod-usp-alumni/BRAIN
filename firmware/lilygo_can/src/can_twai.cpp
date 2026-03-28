#include "can_twai.h"
#include <esp_timer.h>

CanTwai::CanTwai(int tx_pin, int rx_pin) : _tx(tx_pin), _rx(rx_pin) {}

bool CanTwai::timingForBitrate(uint32_t bitrate, twai_timing_config_t& out) {
  switch (bitrate) {
    // Bitrates automotivos comuns
#if defined(TWAI_TIMING_CONFIG_125KBITS)
    case 125000: out = TWAI_TIMING_CONFIG_125KBITS(); return true;
#endif
    case 250000: out = TWAI_TIMING_CONFIG_250KBITS(); return true;
    case 500000: out = TWAI_TIMING_CONFIG_500KBITS(); return true;
#if defined(TWAI_TIMING_CONFIG_800KBITS)
    case 800000: out = TWAI_TIMING_CONFIG_800KBITS(); return true;
#endif
    case 1000000: out = TWAI_TIMING_CONFIG_1MBITS(); return true;
    default:
      // fallback (mais comum em OBD é 500k)
      return false;
  }
}

bool CanTwai::begin(uint32_t bitrate, bool listen_only) {
  if (_started) {
    end();
  }

  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)_tx, (gpio_num_t)_rx,
      listen_only ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL);

  // Aumenta as filas internas do driver TWAI.
  // Motivo: durante tráfego intenso (ou cenários de ataque tipo flood/DoS)
  // a fila padrão pode encher rapidamente, causando rx_missed_count/rx_overrun_count.
  // Esses valores são um bom indicador da qualidade da coleta.
  g_config.tx_queue_len = 10;
  g_config.rx_queue_len = 50;

  twai_timing_config_t t_config;
  if (!timingForBitrate(bitrate, t_config)) {
    return false;
  }

  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
  if (err != ESP_OK) return false;

  err = twai_start();
  if (err != ESP_OK) {
    twai_driver_uninstall();
    return false;
  }

  // Habilitar alertas (opcional)
  // twai_reconfigure_alerts(TWAI_ALERT_ERR_PASS | TWAI_ALERT_BUS_ERROR | TWAI_ALERT_RX_QUEUE_FULL, NULL);

  _started = true;
  _bitrate = bitrate;
  _listenOnly = listen_only;
  return true;
}

bool CanTwai::initiateRecovery() {
  if (!_started) return false;
  // twai_initiate_recovery() só terá efeito quando o estado estiver BUS_OFF.
  // Se chamado fora disso, normalmente retorna ESP_ERR_INVALID_STATE.
  esp_err_t err = twai_initiate_recovery();
  return err == ESP_OK;
}

void CanTwai::end() {
  if (!_started) return;
  twai_stop();
  twai_driver_uninstall();
  _started = false;

  _bitrate = 0;
  _listenOnly = false;
}

bool CanTwai::send(const CanFrame& f, uint32_t timeout_ms) {
  if (!_started) return false;

  // Guard rail: se o driver não está em RUNNING, não tente enfileirar TX.
  // Isso evita encher a fila (msgs_to_tx) durante BUS_OFF/RECOVERY e reduz risco
  // de asserts internos do driver em condições de barramento degradadas.
  twai_status_info_t info;
  if (twai_get_status_info(&info) == ESP_OK) {
    if (info.state != TWAI_STATE_RUNNING) {
      return false;
    }
    // Se a fila de TX já está cheia, evita bloquear/enfileirar mais.
    // (tx_queue_len=10 e existe 1 buffer interno, então 10+1 pode aparecer.)
    if (info.msgs_to_tx >= 10) {
      return false;
    }
  }

  twai_message_t msg = {};
  msg.identifier = f.id;
  msg.extd = f.ext ? 1 : 0;
  msg.rtr = f.rtr ? 1 : 0;
  msg.data_length_code = f.dlc;
  for (uint8_t i = 0; i < f.dlc && i < 8; i++) {
    msg.data[i] = f.data[i];
  }

  esp_err_t err = twai_transmit(&msg, pdMS_TO_TICKS(timeout_ms));
  return err == ESP_OK;
}

bool CanTwai::receive(CanFrame& out, uint32_t timeout_ms) {
  if (!_started) return false;

  twai_message_t msg = {};
  esp_err_t err = twai_receive(&msg, pdMS_TO_TICKS(timeout_ms));
  if (err != ESP_OK) return false;

  out.ts_us = (uint64_t)esp_timer_get_time();
  out.id = msg.identifier;
  out.ext = msg.extd;
  out.rtr = msg.rtr;
  out.dlc = msg.data_length_code;
  for (uint8_t i = 0; i < out.dlc && i < 8; i++) {
    out.data[i] = msg.data[i];
  }
  return true;
}

bool CanTwai::getStatus(CanStatus& st) {
  if (!_started) return false;

  twai_status_info_t info;
  esp_err_t err = twai_get_status_info(&info);
  if (err != ESP_OK) return false;

  st.state = (int)info.state;
  st.bus_error_count = info.bus_error_count;
  st.rx_missed_count = info.rx_missed_count;
st.rx_overrun_count = info.rx_overrun_count;

// Contadores extra (muito úteis para diagnosticar falta de ACK vs erro de recepção)
st.tx_error_counter = info.tx_error_counter;
st.rx_error_counter = info.rx_error_counter;
st.tx_failed_count  = info.tx_failed_count;
st.arb_lost_count   = info.arb_lost_count;
st.msgs_to_tx       = info.msgs_to_tx;
st.msgs_to_rx       = info.msgs_to_rx;
return true;
}
