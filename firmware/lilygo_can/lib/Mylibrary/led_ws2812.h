#pragma once
#include <Arduino.h>

class LedWs2812 {
public:
  void begin();
  void setError(bool enabled);
  void pulseRx();              // pisca azul rapidamente
  void loop();

private:
  bool _error = false;
  uint32_t _lastRxPulseMs = 0;

  // Para piscar vermelho
  uint32_t _lastBlinkMs = 0;
  bool _blinkOn = false;

  void showColor(uint8_t r, uint8_t g, uint8_t b);
};
