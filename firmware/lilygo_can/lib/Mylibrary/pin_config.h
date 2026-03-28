#pragma once

// Pinout (LILYGO T-CAN485 / ESP32)
// Fonte: documentação do produto indica WS2812 RGB em GPIO04, e exemplos oficiais usam estes pinos.

#define WS2812B_DATA   4

// CAN (TWAI)
#define CAN_TX         27
#define CAN_RX         26
#define CAN_SPEED_MODE 23

// RS485 and CAN Boost power supply enable
#define ME2107_EN      16

// microSD (SPI)
#define SD_MISO        2
#define SD_MOSI        15
#define SD_SCLK        14
#define SD_CS          13
