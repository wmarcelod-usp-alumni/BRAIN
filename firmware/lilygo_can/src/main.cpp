#include <Arduino.h>
#include <esp_timer.h>

// NOTE (pesquisa / dissertação):
// Este firmware foi pensado para coletar tráfego CAN com o máximo de robustez possível,
// sem depender de um computador conectado. Por isso, além do Serial, ele:
//  - grava logs no SD (raw + decodificado),
//  - expõe um portal Wi‑Fi local (AP) para checar status e baixar arquivos,
//  - usa um LED WS2812 para feedback imediato (RX/erro).

#include "pin_config.h"
#include "app_config.h"
#include "types.h"

#include "can_twai.h"
#include "sd_logger.h"
#include "led_ws2812.h"
#include "frame_ring.h"
#include "obd_poller.h"
#include "web_portal.h"

static RuntimeStats g_stats;
static LedWs2812    g_led;
static SdLogger     g_sd;
static CanTwai      g_can(CAN_TX, CAN_RX);
static FrameRing    g_ring(128);
static ObdPoller    g_obd(g_can, g_sd, &g_stats);
static WebPortal    g_web(g_stats, g_ring, g_sd, g_can, g_obd);

static uint32_t g_lastFpsMs = 0;
static uint32_t g_lastRxFrames = 0;
static uint32_t g_lastBusErr = 0;

// Flags de erro para LED (evita "apagar" um erro de SD quando o CAN se recupera, por exemplo).
static bool g_sdError  = false;
static bool g_canError = false;

// Controle de tentativa de recovery em BUS_OFF.
static bool     g_busOffRecovering = false;
static uint32_t g_busOffSinceMs    = 0;

static inline void updateLedError() {
  g_led.setError(g_sdError || g_canError);
}

static void printBanner() {
  Serial.println();
  Serial.println("=== CAN OBD Logger (ESP32 / TWAI) ===");
  Serial.printf("CAN bitrate: %lu\n", (unsigned long)AppConfig::CAN_BITRATE);
  Serial.printf("Listen-only: %s\n", AppConfig::CAN_LISTEN_ONLY ? "YES" : "NO");
  Serial.printf("WiFi AP SSID: %s\n", AppConfig::WIFI_SSID);
  Serial.println("====================================");
}

void setup() {
  Serial.begin(115200);
  delay(200);

  printBanner();

  // Alimentação/boost do T-CAN485
  pinMode(ME2107_EN, OUTPUT);
  digitalWrite(ME2107_EN, HIGH);

  // Modo de velocidade do transceiver (se aplicável)
  pinMode(CAN_SPEED_MODE, OUTPUT);
  digitalWrite(CAN_SPEED_MODE, AppConfig::CAN_SPEED_MODE_LEVEL);

  // LED
  g_led.begin();

  // SD
  g_stats.sd_ok = g_sd.begin();
  if (!g_stats.sd_ok) {
    Serial.println("[SD] Falha ao inicializar SD.");
    g_sdError = true;
    updateLedError();
  } else {
    Serial.println("[SD] OK.");
    // Marca início de sessão para facilitar análise offline.
    g_sd.logEvent((uint64_t)esp_timer_get_time(), "boot", "logger started");
  }

  // CAN
  Serial.println("[CAN] Init TWAI ...");
  bool canOk = g_can.begin(AppConfig::CAN_BITRATE, AppConfig::CAN_LISTEN_ONLY);
  if (!canOk) {
    Serial.println("[CAN] ERRO ao inicializar TWAI.");
    g_canError = true;
    updateLedError();
  } else {
    Serial.println("[CAN] OK.");
    // Inicializa estado (para evitar enviar OBD antes de sabermos que está RUNNING)
    (void)g_can.getStatus(g_stats.can);
  }

  // OBD Poller
  g_obd.begin();

  // Portal Wi‑Fi (somente leitura / download; sem aplicar configurações via web)
  g_web.begin();

  g_lastFpsMs = millis();
}

void loop() {
  // 1) RX CAN (esvazia a fila rapidamente)
  CanFrame f;
  while (g_can.receive(f, 0)) {
    g_stats.rx_frames++;
    g_ring.push(f);
    g_led.pulseRx();

    // log raw (RX)
    if (g_stats.sd_ok) {
      g_sd.logCanFrame(f, 'R');
    }

    // tenta decodificar respostas OBD/UDS
    g_obd.onRxFrame(f);
  }

  // 2) Poller (TX OBD/UDS)
  // Evita enviar diagnóstico se o controlador CAN não estiver em RUNNING.
  // (O status é atualizado em bloco, mais abaixo, para reduzir overhead por frame.)
  // Envio de diagnóstico somente quando o controlador CAN estiver RUNNING.
  // Obs.: Em modo listen-only, não envia nada.
  if (!g_web.autoDetectRunning() && !g_can.listenOnly() && g_can.started() && g_stats.can.state == TWAI_STATE_RUNNING) {
    g_obd.loop();
  }

  // 3) SD flush
  if (g_stats.sd_ok) {
    g_sd.loop();

    // Detecta falha em runtime (cartão removido, FS corrompido, etc.)
    if (!g_sd.ok()) {
      g_stats.sd_ok = false;
      g_sdError = true;
      updateLedError();
      Serial.println("[SD] ERRO: logger entrou em estado NOK (falha de escrita/arquivo).");
    }
  }

  // 4) Portal Wi‑Fi
  g_web.loop();

  // 5) Estatísticas no serial (1x por segundo)
  const uint32_t now = millis();
  if (now - g_lastFpsMs >= AppConfig::STATS_PRINT_MS) {
    g_lastFpsMs = now;

    // Atualiza status do CAN 1x/seg (bem mais leve do que a cada frame).
    if (g_can.started()) {
      (void)g_can.getStatus(g_stats.can);
    }

    uint32_t rxDelta = g_stats.rx_frames - g_lastRxFrames;
    g_lastRxFrames = g_stats.rx_frames;
    g_stats.fps = rxDelta;

    uint32_t busErr = g_stats.can.bus_error_count;
    uint32_t busErrDelta = busErr - g_lastBusErr;
    g_lastBusErr = busErr;

    Serial.printf(
              "[stats] fps=%lu state=%d tx_err=%lu rx_err=%lu tx_fail=%lu arb_lost=%lu rx_missed=%lu rx_overrun=%lu bus_err=%lu (+%lu) tx=%lu\n",
              (unsigned long)g_stats.fps,
              g_stats.can.state,
              (unsigned long)g_stats.can.tx_error_counter,
              (unsigned long)g_stats.can.rx_error_counter,
              (unsigned long)g_stats.can.tx_failed_count,
              (unsigned long)g_stats.can.arb_lost_count,
              (unsigned long)g_stats.can.rx_missed_count,
              (unsigned long)g_stats.can.rx_overrun_count,
              (unsigned long)g_stats.can.bus_error_count,
              (unsigned long)busErrDelta,
              (unsigned long)g_stats.tx_frames);
// Loga stats no SD para auditoria offline (útil em coleta sem PC).
    if (g_stats.sd_ok) {
      g_sd.logRuntimeStats(now, g_stats);
    }

    // Recovery automático em BUS_OFF.
    // Observação importante: em algumas versões do driver TWAI (Arduino core/IDF),
    // chamar twai_initiate_recovery() enquanto há mensagens pendentes na fila de TX
    // pode levar a assert interno (ex.: tx_msg_count < 0). Para aumentar robustez
    // em cenários de barramento degradado (ataque/fiação/bitrate), aqui fazemos
    // um "hard reset" do driver (stop/uninstall + install/start), o que também
    // limpa a fila de TX.
    if (!g_can.started()) {
      // Se o driver CAN não está ativo, mantém erro e não tenta recovery.
      g_canError = true;
      updateLedError();
    } else if (g_stats.can.state == TWAI_STATE_BUS_OFF) {
      // BUS_OFF: erro grave no CAN. Tenta recuperação automática.
      g_canError = true;
      updateLedError();

      if (!g_busOffRecovering) {
        g_busOffRecovering = true;
        g_busOffSinceMs = now;
        Serial.println("[CAN] BUS_OFF detectado. Aguardando para iniciar recovery...");
      }

      // Aguarda 3s antes da primeira tentativa (tempo suficiente para condição de recuperação)
      if (now - g_busOffSinceMs >= 3000) {
        Serial.println("[CAN] BUS_OFF: reiniciando driver TWAI (hard reset)...");

        // Para garantir que nada seja enfileirado enquanto reinicia
        g_obd.reset();

        // Hard reset do driver
        g_can.end();
        delay(50);

        const bool ok = g_can.begin(AppConfig::CAN_BITRATE, AppConfig::CAN_LISTEN_ONLY);
        if (ok) {
          (void)g_can.getStatus(g_stats.can);
          Serial.println("[CAN] TWAI reiniciado.");
        } else {
          Serial.println("[CAN] Falha ao reiniciar TWAI.");
        }

        // Reagenda nova tentativa mais à frente, se continuar em BUS_OFF.
        g_busOffSinceMs = now;
      }
    } else {
      // Saiu de BUS_OFF (RUNNING ou RECOVERY ou STOPPED)
      if (g_busOffRecovering && g_stats.can.state == TWAI_STATE_RUNNING) {
        Serial.println("[CAN] Recovery concluído, controlador voltou a RUNNING.");
        g_busOffRecovering = false;
      }

      // Considera CAN saudável apenas quando está RUNNING.
      // (RECOVERY/STOPPED continuam sendo condições de atenção).
      g_canError = (g_stats.can.state != TWAI_STATE_RUNNING);
      updateLedError();
    }
  }

  // 6) LED
  g_led.loop();
}
