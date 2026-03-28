#pragma once
#include <Arduino.h>
#include "driver/twai.h"
#include "types.h"

class CanTwai {
public:
  CanTwai(int tx_pin, int rx_pin);

  bool begin(uint32_t bitrate, bool listen_only);
  void end();

  bool send(const CanFrame& f, uint32_t timeout_ms = 20);
  bool receive(CanFrame& out, uint32_t timeout_ms = 0);

  bool getStatus(CanStatus& st);

  // Config atual (útil para UI / diagnóstico)
  uint32_t bitrate() const { return _bitrate; }
  bool listenOnly() const { return _listenOnly; }

  // Indica se o driver TWAI foi inicializado e está ativo.
  bool started() const { return _started; }

  // Solicita recuperação quando em BUS_OFF. Retorna false se o driver não estiver iniciado.
  bool initiateRecovery();

private:
  int _tx;
  int _rx;
  bool _started = false;

  uint32_t _bitrate = 0;
  bool _listenOnly = false;

  static bool timingForBitrate(uint32_t bitrate, twai_timing_config_t& out);
};
