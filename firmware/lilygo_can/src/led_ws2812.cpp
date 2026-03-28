#include "led_ws2812.h"
#include <FastLED.h>
#include "pin_config.h"

#define NUM_LEDS 1
static CRGB leds[NUM_LEDS];

static constexpr uint8_t BRIGHTNESS = 50;
static constexpr uint32_t RX_PULSE_MS = 60;
static constexpr uint32_t ERR_BLINK_MS = 300;

static uint8_t lastR = 255, lastG = 255, lastB = 255; // força primeira atualização

void LedWs2812::begin() {
  FastLED.addLeds<WS2812B, WS2812B_DATA, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  showColor(0, 32, 0); // verde inicial
}

void LedWs2812::setError(bool enabled) {
  _error = enabled;
}

void LedWs2812::pulseRx() {
  _lastRxPulseMs = millis();
}

void LedWs2812::showColor(uint8_t r, uint8_t g, uint8_t b) {
  if (r == lastR && g == lastG && b == lastB) return;
  lastR = r; lastG = g; lastB = b;
  leds[0] = CRGB(r, g, b);
  FastLED.show();
}

void LedWs2812::loop() {
  const uint32_t now = millis();

  if (_error) {
    // piscar vermelho
    if (now - _lastBlinkMs >= ERR_BLINK_MS) {
      _lastBlinkMs = now;
      _blinkOn = !_blinkOn;
      if (_blinkOn) showColor(32, 0, 0);
      else          showColor(0, 0, 0);
    }
    return;
  }

  // Se houve RX recente, pisca azul por RX_PULSE_MS
  if (now - _lastRxPulseMs <= RX_PULSE_MS) {
    showColor(0, 0, 32);
  } else {
    // verde steady (OK)
    showColor(0, 32, 0);
  }
}
