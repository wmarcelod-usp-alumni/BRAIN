#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

#include "types.h"
#include "frame_ring.h"
#include "sd_logger.h"
#include "can_twai.h"
#include "obd_poller.h"

class WebPortal {
public:
  WebPortal(RuntimeStats& stats, FrameRing& ring, SdLogger& sd, CanTwai& can, ObdPoller& obd);

  void begin();
  void loop();

  // Para o loop principal: evita enviar OBD/UDS durante auto-detecção.
  bool autoDetectRunning() const { return _ad.running; }

private:
  RuntimeStats& _stats;
  FrameRing& _ring;
  SdLogger& _sd;
  CanTwai& _can;
  ObdPoller& _obd;

  DNSServer _dns;
  WebServer _srv;

  // -------- Auto-detecção de bitrate + protocolo --------
  struct AutoDetectState {
    bool running = false;
    bool found = false;
    bool stopped = false;

    uint32_t dwell_ms = 2000;     // tempo escutando por tentativa
    uint32_t probe_ms = 600;      // janela para resposta ao probe OBD
    uint32_t min_frames = 5;      // frames mínimos para considerar bitrate "ativo"

    uint32_t prev_bitrate = 0;
    bool     prev_listen_only = false;
    ObdPoller::Protocol prev_proto = ObdPoller::Protocol::OBD11;

    size_t step = 0;
    size_t total = 0;
    uint32_t current_bitrate = 0;
    ObdPoller::Protocol current_proto = ObdPoller::Protocol::OBD11;

    uint32_t rx_start = 0;
    uint32_t step_start_ms = 0;

    // 0=IDLE, 1=LISTEN, 2=PROBE_WAIT
    uint8_t phase = 0;

    uint32_t probe_start_ms = 0;
    uint64_t probe_ts_us = 0;

    uint32_t found_bitrate = 0;
    ObdPoller::Protocol found_proto = ObdPoller::Protocol::OBD11;

    char last_msg[96] = {0};
  };

  AutoDetectState _ad;

  // Upload
  File _uploadFile;
  String _uploadPath;

  void setupRoutes();

  // UI
  void handleRoot();
  void handleConfigPage();
  void handleStatus();
  void handleFrames();
  void handleFiles();
  void handleDownload();
  void handleMark();

  // Config API
  void handleApiConfig();
  void handleApiSetConfig();

  // Auto-detect API
  void handleApiAutoDetectStart();
  void handleApiAutoDetectStop();
  void handleApiAutoDetectStatus();
  void autoDetectLoop();
  void autoDetectStart(uint32_t dwell_ms, uint32_t probe_ms, uint32_t min_frames);
  void autoDetectStop(bool restorePrevious);

  // SD manager
  void handleDelete();
  void handleRename();
  void handleMkdir();
  void handleUploadPost();
  void handleUpload();
  void handleNotFound();


  String htmlHeader(const char* title, int refreshSeconds = -1) const;
  String htmlFooter() const;

  // helpers
  static String normalizePath(String p);
  static bool isSafePath(const String& p);
  static String urlEncode(const String& s);
  bool isProtectedPath(const String& path) const;
  bool applyCanConfig(uint32_t bitrate, bool listen_only, ObdPoller::Protocol proto);
};
