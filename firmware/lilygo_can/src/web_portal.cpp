#include "web_portal.h"
#include "app_config.h"

#include <SD.h>
#include <esp_timer.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const byte DNS_PORT = 53;
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_NETMASK(255, 255, 255, 0);

// -------------------- Helpers locais --------------------
static const char* protoToStr(ObdPoller::Protocol p) {
  switch (p) {
    case ObdPoller::Protocol::RAW:   return "raw";
    case ObdPoller::Protocol::OBD11: return "obd11";
    case ObdPoller::Protocol::OBD29: return "obd29";
    default:                         return "unknown";
  }
}

static const char* protoToLabel(ObdPoller::Protocol p) {
  switch (p) {
    case ObdPoller::Protocol::RAW:   return "Somente monitoramento (sem TX)";
    case ObdPoller::Protocol::OBD11: return "OBD-II CAN 11-bit (0x7DF / 0x7E8..0x7EF)";
    case ObdPoller::Protocol::OBD29: return "OBD-II CAN 29-bit (0x18DB33F1 / 0x18DAF1xx)";
    default:                         return "Desconhecido";
  }
}

static ObdPoller::Protocol parseProto(const String& s) {
  String v = s;
  v.toLowerCase();
  if (v == "0" || v == "raw") return ObdPoller::Protocol::RAW;
  if (v == "1" || v == "obd11" || v == "11") return ObdPoller::Protocol::OBD11;
  if (v == "2" || v == "obd29" || v == "29") return ObdPoller::Protocol::OBD29;
  return ObdPoller::Protocol::OBD11;
}

static uint32_t parseU32(const String& s, uint32_t defVal = 0) {
  if (s.length() == 0) return defVal;
  char* endp = nullptr;
  unsigned long v = strtoul(s.c_str(), &endp, 0); // base 0 => aceita 0x..
  if (endp == s.c_str()) return defVal;
  return (uint32_t)v;
}

static String toHex(uint32_t v) {
  char buf[16];
  snprintf(buf, sizeof(buf), "0x%lX", (unsigned long)v);
  return String(buf);
}

static String toHex16(uint16_t v) {
  char buf[16];
  snprintf(buf, sizeof(buf), "0x%X", (unsigned)v);
  return String(buf);
}

WebPortal::WebPortal(RuntimeStats& stats, FrameRing& ring, SdLogger& sd, CanTwai& can, ObdPoller& obd)
: _stats(stats), _ring(ring), _sd(sd), _can(can), _obd(obd), _srv(AppConfig::WIFI_PORT) {}

String WebPortal::htmlHeader(const char* title, int refreshSeconds) const {
  String html;
  html.reserve(768);
  html += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  if (refreshSeconds > 0) {
    html += "<meta http-equiv='refresh' content='";
    html += String(refreshSeconds);
    html += "'>";
  }
  html += "<title>";
  html += title;
  html += "</title>";

  // CSS simples, sem frameworks
  html += "<style>";
  html += "body{font-family:Arial,Helvetica,sans-serif;margin:16px;}";
  html += "code{background:#f2f2f2;padding:2px 4px;border-radius:3px;}";
  html += "table{border-collapse:collapse;width:100%;max-width:980px;}";
  html += "th,td{border:1px solid #ddd;padding:8px;}";
  html += "th{background:#f7f7f7;text-align:left;}";
  html += "a{color:#0b57d0;text-decoration:none;}";
  html += "a:hover{text-decoration:underline;}";
  html += ".row{max-width:980px;}";
  html += ".pill{display:inline-block;padding:2px 8px;border:1px solid #ddd;border-radius:999px;background:#fafafa;}";
  html += ".warn{color:#8a3d00;}";
  html += ".err{color:#b00020;}";
  html += ".ok{color:#137333;}";
  html += "</style>";

  html += "</head><body><div class='row'>";
  return html;
}

String WebPortal::htmlFooter() const {
  return "</div></body></html>";
}

void WebPortal::begin() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);
  WiFi.softAP(AppConfig::WIFI_SSID, AppConfig::WIFI_PASS);

  _dns.start(DNS_PORT, "*", AP_IP);

  setupRoutes();
  _srv.begin();
}

void WebPortal::setupRoutes() {
  _srv.on("/", HTTP_GET, [this]() { handleRoot(); });
  _srv.on("/config", HTTP_GET, [this]() { handleConfigPage(); });

  _srv.on("/api/status", HTTP_GET, [this]() { handleStatus(); });
  _srv.on("/api/frames", HTTP_GET, [this]() { handleFrames(); });
  _srv.on("/api/mark", HTTP_GET, [this]() { handleMark(); });

  _srv.on("/files", HTTP_GET, [this]() { handleFiles(); });
  _srv.on("/download", HTTP_GET, [this]() { handleDownload(); });

  // SD manager
  _srv.on("/delete", HTTP_GET, [this]() { handleDelete(); });
  _srv.on("/rename", HTTP_GET, [this]() { handleRename(); });
  _srv.on("/mkdir", HTTP_GET, [this]() { handleMkdir(); });

  // Upload: handler final + callback de streaming
  _srv.on("/upload", HTTP_POST,
          [this]() { handleUploadPost(); },
          [this]() { handleUpload(); });

  // Config API
  _srv.on("/api/config", HTTP_GET, [this]() { handleApiConfig(); });
  _srv.on("/api/config/set", HTTP_GET, [this]() { handleApiSetConfig(); });

  // Auto-detect API
  _srv.on("/api/autodetect/start", HTTP_GET, [this]() { handleApiAutoDetectStart(); });
  _srv.on("/api/autodetect/stop", HTTP_GET, [this]() { handleApiAutoDetectStop(); });
  _srv.on("/api/autodetect", HTTP_GET, [this]() { handleApiAutoDetectStatus(); });

  _srv.onNotFound([this]() { handleNotFound(); });
}

void WebPortal::loop() {
  _dns.processNextRequest();
  _srv.handleClient();
  autoDetectLoop();
}

// -------------------- Helpers (paths / strings) --------------------

String WebPortal::normalizePath(String p) {
  p.trim();
  if (p.length() == 0) return String("/");
  if (!p.startsWith("/")) p = "/" + p;

  // remove duplications of '/'
  while (p.indexOf("//") >= 0) {
    p.replace("//", "/");
  }

  // remove trailing slash (except root)
  if (p.length() > 1 && p.endsWith("/")) {
    p.remove(p.length() - 1);
  }

  return p;
}

bool WebPortal::isSafePath(const String& p) {
  if (p.length() == 0) return false;
  if (!p.startsWith("/")) return false;
  if (p.indexOf("..") >= 0) return false;
  if (p.indexOf('\\') >= 0) return false;
  // evita caminhos absurdamente longos
  if (p.length() > 128) return false;
  return true;
}

String WebPortal::urlEncode(const String& s) {
  static const char hex[] = "0123456789ABCDEF";
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    const uint8_t c = (uint8_t)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/' ) {
      out += (char)c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

bool WebPortal::isProtectedPath(const String& path) const {
  String p = normalizePath(path);
  if (p == "/" || p == "/logs") return true;

  // Protege arquivos abertos nesta sessão
  if (p == normalizePath(_sd.rawPath())) return true;
  if (p == normalizePath(_sd.obdPath())) return true;
  if (p == normalizePath(_sd.udsPath())) return true;
  if (p == normalizePath(_sd.statsPath())) return true;
  if (p == normalizePath(_sd.eventsPath())) return true;
  return false;
}

// Aplica bitrate + listenOnly + protocolo (somente OBD profile). UDS é configurado fora.
bool WebPortal::applyCanConfig(uint32_t bitrate, bool listen_only, ObdPoller::Protocol proto) {
  if (bitrate == 0) return false;

  // O portal pode reconfigurar o TWAI em runtime.
  if (!_can.begin(bitrate, listen_only)) {
    return false;
  }

  _obd.setProtocol(proto);

  // Para um modo "sniff" puro, o usuário deve desabilitar UDS também.
  if (proto == ObdPoller::Protocol::RAW) {
    _obd.setObdEnabled(false);
  } else {
    _obd.setObdEnabled(true);
  }
  _obd.reset();

  // Atualiza status imediatamente para UI
  (void)_can.getStatus(_stats.can);
  return true;
}

// -------------------- UI: Root / Config --------------------

void WebPortal::handleRoot() {
  String html = htmlHeader("CAN Logger", 5);

  html += "<h2>CAN Logger</h2>";
  html += "<p><span class='pill'>AP IP: ";
  html += AP_IP.toString();
  html += "</span></p>";

  // Status resumido
  html += "<h3>Status</h3>";
  html += "<ul>";
  html += "<li>CAN state: <b>" + String(twaiStateToStr(_stats.can.state)) + "</b></li>";
  html += "<li>CAN bitrate: <b>" + String((unsigned long)_can.bitrate()) + "</b> bps</li>";
  html += "<li>Modo: <b>" + String(_can.listenOnly() ? "LISTEN_ONLY" : "NORMAL") + "</b></li>";
  html += "<li>Protocolo diag: <b>" + String(protoToStr(_obd.protocol())) + "</b></li>";
  html += "<li>UDS: <b>" + String(_obd.udsEnabled() ? "ON" : "OFF") + "</b> (req=" + toHex(_obd.udsReqId()) + ", resp=" + toHex(_obd.udsRespId()) + ", did=" + toHex16(_obd.udsDid()) + ")</li>";
  html += "<li>RX frames: <b>" + String((unsigned long)_stats.rx_frames) + "</b> (fps=" + String((unsigned long)_stats.fps) + ")</li>";
  html += "<li>TX frames: <b>" + String((unsigned long)_stats.tx_frames) + "</b></li>";
  html += "<li>BUS errors: <b>" + String((unsigned long)_stats.can.bus_error_count) + "</b></li>";
  html += "<li>SD: <b class='" + String(_stats.sd_ok ? "ok" : "err") + "'>" + String(_stats.sd_ok ? "OK" : "NOK") + "</b></li>";
  html += "</ul>";

  if (_ad.running) {
    html += "<p class='warn'><b>Auto-detect em andamento</b> — acesse <a href='/config'>/config</a> para ver progresso.</p>";
  }

  html += "<h3>Links</h3>";
  html += "<ul>";
  html += "<li><a href='/config'>Configurar CAN/OBD + Auto-detect</a></li>";
  html += "<li><a href='/files'>Gerenciar arquivos do SD</a></li>";
  html += "<li><a href='/api/status'>/api/status</a> (JSON)</li>";
  html += "<li><a href='/api/frames'>/api/frames</a> (últimos frames)</li>";
  html += "</ul>";

  html += htmlFooter();
  _srv.send(200, "text/html", html);
}

void WebPortal::handleConfigPage() {
  // Se estiver rodando autodetect, faz refresh mais rápido para mostrar progresso
  const int refresh = _ad.running ? 2 : -1;
  String html = htmlHeader("Config", refresh);

  html += "<h2>Configuração CAN/OBD</h2>";
  html += "<p><a href='/'>Voltar</a> | <a href='/files'>Arquivos SD</a></p>";

  if (_srv.hasArg("msg")) {
    html += "<p class='pill'>" + _srv.arg("msg") + "</p>";
  }

  // Form de configuração
  html += "<h3>Aplicar configuração</h3>";
  html += "<form method='GET' action='/api/config/set'>";

  // Bitrates comuns
  const uint32_t currentRate = _can.bitrate();
  html += "<label>Bitrate: <select name='bitrate'>";
  const uint32_t rates[] = {125000, 250000, 500000, 800000, 1000000};
  for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
    html += "<option value='" + String((unsigned long)rates[i]) + "'";
    if (rates[i] == currentRate) html += " selected";
    html += ">" + String((unsigned long)rates[i]) + "</option>";
  }
  html += "</select></label><br><br>";

  // Protocolo
  ObdPoller::Protocol curProto = _obd.protocol();
  html += "<label>Protocolo: <select name='proto'>";
  const ObdPoller::Protocol protos[] = {ObdPoller::Protocol::RAW, ObdPoller::Protocol::OBD11, ObdPoller::Protocol::OBD29};
  for (size_t i = 0; i < sizeof(protos) / sizeof(protos[0]); i++) {
    html += "<option value='" + String(protoToStr(protos[i])) + "'";
    if (protos[i] == curProto) html += " selected";
    html += ">" + String(protoToLabel(protos[i])) + "</option>";
  }
  html += "</select></label><br><br>";

  // Listen-only
  html += "<label><input type='checkbox' name='listen' value='1'";
  if (_can.listenOnly()) html += " checked";
  html += "> Listen-only (não transmite / não ACK)</label><br><br>";

  // UDS
  html += "<fieldset style='max-width:980px;border:1px solid #ddd;padding:12px;'>";
  html += "<legend>UDS (service 0x22)</legend>";
  html += "<label><input type='checkbox' name='uds' value='1'";
  if (_obd.udsEnabled()) html += " checked";
  html += "> Habilitar UDS</label><br><br>";

  html += "<label>UDS req id: <input name='uds_req' value='" + toHex(_obd.udsReqId()) + "'></label><br>";
  html += "<label>UDS resp id: <input name='uds_resp' value='" + toHex(_obd.udsRespId()) + "'></label><br>";
  html += "<label>UDS DID: <input name='uds_did' value='" + toHex16(_obd.udsDid()) + "'></label><br>";
  html += "<label><input type='checkbox' name='uds_ext' value='1'";
  if (_obd.udsExt()) html += " checked";
  html += "> IDs UDS são 29-bit (extended)</label>";

  html += "</fieldset><br>";

  html += "<input type='submit' value='Aplicar'>";
  html += "</form>";

  // Auto-detect
  html += "<h3>Auto-detect (bitrate + protocolo)</h3>";
  if (_ad.running) {
    html += "<p class='warn'><b>Rodando:</b> etapa " + String((unsigned long)(_ad.step + 1)) + "/" + String((unsigned long)_ad.total) +
            " — testando bitrate=" + String((unsigned long)_ad.current_bitrate) +
            ", proto=" + String(protoToStr(_ad.current_proto)) + "</p>";
    if (_ad.last_msg[0]) {
      html += "<p><code>" + String(_ad.last_msg) + "</code></p>";
    }
    html += "<p><a href='/api/autodetect/stop'>Parar auto-detect</a></p>";
  } else {
    if (_ad.last_msg[0]) {
      html += "<p><code>" + String(_ad.last_msg) + "</code></p>";
    }
    html += "<form method='GET' action='/api/autodetect/start'>";
    html += "<label>Dwell por tentativa (ms): <input name='dwell' value='" + String((unsigned long)_ad.dwell_ms) + "'></label><br>";
    html += "<label>Timeout do probe OBD (ms): <input name='probe' value='" + String((unsigned long)_ad.probe_ms) + "'></label><br>";
    html += "<label>Frames mínimos p/ considerar bitrate ativo: <input name='min_frames' value='" + String((unsigned long)_ad.min_frames) + "'></label><br><br>";
    html += "<input type='submit' value='Iniciar auto-detect'>";
    html += "</form>";
  }

  html += htmlFooter();
  _srv.send(200, "text/html", html);
}

// -------------------- JSON APIs --------------------

void WebPortal::handleStatus() {
  String json;
  json.reserve(512);
  json += "{";
  json += "\"rx_frames\":" + String((unsigned long)_stats.rx_frames) + ",";
  json += "\"tx_frames\":" + String((unsigned long)_stats.tx_frames) + ",";
  json += "\"fps\":" + String((unsigned long)_stats.fps) + ",";
  json += "\"can_state\":\"" + String(twaiStateToStr(_stats.can.state)) + "\",";
  json += "\"tx_err\":" + String((unsigned long)_stats.can.tx_error_counter) + ",";
  json += "\"rx_err\":" + String((unsigned long)_stats.can.rx_error_counter) + ",";
  json += "\"tx_failed\":" + String((unsigned long)_stats.can.tx_failed_count) + ",";
  json += "\"arb_lost\":" + String((unsigned long)_stats.can.arb_lost_count) + ",";
  json += "\"msgs_to_tx\":" + String((unsigned long)_stats.can.msgs_to_tx) + ",";
  json += "\"msgs_to_rx\":" + String((unsigned long)_stats.can.msgs_to_rx) + ",";
  json += "\"bus_err\":" + String((unsigned long)_stats.can.bus_error_count) + ",";
  json += "\"rx_missed\":" + String((unsigned long)_stats.can.rx_missed_count) + ",";
  json += "\"rx_overrun\":" + String((unsigned long)_stats.can.rx_overrun_count) + ",";
  json += "\"sd_ok\":" + String(_stats.sd_ok ? "true" : "false") + ",";
  json += "\"can_bitrate\":" + String((unsigned long)_can.bitrate()) + ",";
  json += "\"listen_only\":" + String(_can.listenOnly() ? "true" : "false") + ",";
  json += "\"proto\":\"" + String(protoToStr(_obd.protocol())) + "\",";
  json += "\"uds_enabled\":" + String(_obd.udsEnabled() ? "true" : "false") + ",";
  json += "\"autodetect_running\":" + String(_ad.running ? "true" : "false");
  json += "}";
  _srv.send(200, "application/json", json);
}

void WebPortal::handleApiConfig() {
  String json;
  json.reserve(768);
  json += "{";
  json += "\"bitrate\":" + String((unsigned long)_can.bitrate()) + ",";
  json += "\"listen_only\":" + String(_can.listenOnly() ? "true" : "false") + ",";
  json += "\"proto\":\"" + String(protoToStr(_obd.protocol())) + "\",";
  json += "\"uds\":{";
  json += "\"enabled\":" + String(_obd.udsEnabled() ? "true" : "false") + ",";
  json += "\"req\":\"" + toHex(_obd.udsReqId()) + "\",";
  json += "\"resp\":\"" + toHex(_obd.udsRespId()) + "\",";
  json += "\"did\":\"" + toHex16(_obd.udsDid()) + "\",";
  json += "\"ext\":" + String(_obd.udsExt() ? "true" : "false");
  json += "},";

  json += "\"autodetect\":{";
  json += "\"running\":" + String(_ad.running ? "true" : "false") + ",";
  json += "\"found\":" + String(_ad.found ? "true" : "false") + ",";
  json += "\"step\":" + String((unsigned long)_ad.step) + ",";
  json += "\"total\":" + String((unsigned long)_ad.total) + ",";
  json += "\"current_bitrate\":" + String((unsigned long)_ad.current_bitrate) + ",";
  json += "\"current_proto\":\"" + String(protoToStr(_ad.current_proto)) + "\",";
  json += "\"msg\":\"" + String(_ad.last_msg) + "\"";
  json += "}";

  json += "}";

  _srv.send(200, "application/json", json);
}

void WebPortal::handleApiSetConfig() {
  // Para evitar interferência com o auto-detect
  if (_ad.running) {
    autoDetectStop(true);
  }

  uint32_t bitrate = parseU32(_srv.arg("bitrate"), _can.bitrate());
  ObdPoller::Protocol proto = parseProto(_srv.arg("proto"));
  bool listen_only = _srv.hasArg("listen") && (_srv.arg("listen") == "1" || _srv.arg("listen") == "on");

  bool uds_en = _srv.hasArg("uds") && (_srv.arg("uds") == "1" || _srv.arg("uds") == "on");
  uint32_t uds_req = parseU32(_srv.arg("uds_req"), _obd.udsReqId());
  uint32_t uds_resp = parseU32(_srv.arg("uds_resp"), _obd.udsRespId());
  uint16_t uds_did = (uint16_t)parseU32(_srv.arg("uds_did"), _obd.udsDid());
  bool uds_ext = _srv.hasArg("uds_ext") && (_srv.arg("uds_ext") == "1" || _srv.arg("uds_ext") == "on");

  if (bitrate == 0) {
    _srv.send(400, "text/plain", "invalid bitrate");
    return;
  }

  // Aplica CAN
  if (!_can.begin(bitrate, listen_only)) {
    _srv.send(500, "text/plain", "failed to start CAN with requested bitrate");
    return;
  }

  // Aplica config do OBD/UDS
  _obd.setProtocol(proto);
  if (proto == ObdPoller::Protocol::RAW) {
    _obd.setObdEnabled(false);
  } else {
    _obd.setObdEnabled(true);
  }

  _obd.setUdsConfig(uds_req, uds_resp, uds_did, uds_ext);
  _obd.setUdsEnabled(uds_en);
  _obd.reset();

  (void)_can.getStatus(_stats.can);

  if (_stats.sd_ok) {
    char msg[128];
    snprintf(msg, sizeof(msg), "bitrate=%lu listen=%d proto=%s uds=%d",
             (unsigned long)bitrate,
             (int)listen_only,
             protoToStr(proto),
             (int)uds_en);
    _sd.logEvent((uint64_t)esp_timer_get_time(), "config", msg);
  }

  _srv.sendHeader("Location", "/config?msg=Config%20aplicada", true);
  _srv.send(302, "text/plain", "");
}

// -------------------- Auto-detect APIs --------------------

void WebPortal::handleApiAutoDetectStart() {
  uint32_t dwell_ms = parseU32(_srv.arg("dwell"), 2000);
  uint32_t probe_ms = parseU32(_srv.arg("probe"), 600);
  uint32_t min_frames = parseU32(_srv.arg("min_frames"), 5);

  autoDetectStart(dwell_ms, probe_ms, min_frames);

  _srv.sendHeader("Location", "/config?msg=Auto-detect%20iniciado", true);
  _srv.send(302, "text/plain", "");
}

void WebPortal::handleApiAutoDetectStop() {
  autoDetectStop(true);
  _srv.sendHeader("Location", "/config?msg=Auto-detect%20parado", true);
  _srv.send(302, "text/plain", "");
}

void WebPortal::handleApiAutoDetectStatus() {
  handleApiConfig();
}

void WebPortal::autoDetectStart(uint32_t dwell_ms, uint32_t probe_ms, uint32_t min_frames) {
  if (_ad.running) {
    // já em execução
    return;
  }

  _ad.running = true;
  _ad.found = false;
  _ad.stopped = false;
  _ad.phase = 0;

  _ad.dwell_ms = dwell_ms;
  _ad.probe_ms = probe_ms;
  _ad.min_frames = min_frames;

  _ad.prev_bitrate = _can.bitrate();
  if (_ad.prev_bitrate == 0) _ad.prev_bitrate = AppConfig::CAN_BITRATE;
  _ad.prev_listen_only = _can.listenOnly();
  _ad.prev_proto = _obd.protocol();

  _ad.step = 0;

  // calcula total
  const uint32_t rates[] = {500000, 250000, 125000, 800000, 1000000};
  const size_t rateCount = sizeof(rates) / sizeof(rates[0]);
  const size_t protoCount = 2; // OBD11, OBD29
  _ad.total = rateCount * protoCount;

  snprintf(_ad.last_msg, sizeof(_ad.last_msg), "Iniciando auto-detect...");
}

void WebPortal::autoDetectStop(bool restorePrevious) {
  if (!_ad.running) {
    snprintf(_ad.last_msg, sizeof(_ad.last_msg), "Auto-detect não está em execução.");
    return;
  }

  _ad.running = false;
  _ad.stopped = true;
  _ad.phase = 0;

  snprintf(_ad.last_msg, sizeof(_ad.last_msg), "Auto-detect parado pelo usuário.");

  if (restorePrevious) {
    (void)applyCanConfig(_ad.prev_bitrate, _ad.prev_listen_only, _ad.prev_proto);
  }
}

void WebPortal::autoDetectLoop() {
  if (!_ad.running) return;

  // Lista fixa: velocidades comuns + protocolos OBD11/OBD29
  const uint32_t rates[] = {500000, 250000, 125000, 800000, 1000000};
  const size_t rateCount = sizeof(rates) / sizeof(rates[0]);
  const ObdPoller::Protocol protos[] = {ObdPoller::Protocol::OBD11, ObdPoller::Protocol::OBD29};
  const size_t protoCount = sizeof(protos) / sizeof(protos[0]);

  if (_ad.total == 0) {
    _ad.total = rateCount * protoCount;
  }

  // terminou sem sucesso
  if (_ad.step >= _ad.total) {
    _ad.running = false;
    snprintf(_ad.last_msg, sizeof(_ad.last_msg), "Auto-detect falhou: nenhum bitrate/protocolo respondeu.");
    (void)applyCanConfig(_ad.prev_bitrate, _ad.prev_listen_only, _ad.prev_proto);
    return;
  }

  // Estado 0: iniciar uma nova etapa
  if (_ad.phase == 0) {
    const size_t rateIdx = _ad.step / protoCount;
    const size_t protoIdx = _ad.step % protoCount;

    _ad.current_bitrate = rates[rateIdx];
    _ad.current_proto = protos[protoIdx];

    // Inicializa CAN em listen-only para não perturbar barramento
    if (!_can.begin(_ad.current_bitrate, true)) {
      snprintf(_ad.last_msg, sizeof(_ad.last_msg), "Bitrate %lu não suportado pelo build.", (unsigned long)_ad.current_bitrate);
      _ad.step++;
      _ad.phase = 0;
      return;
    }
    (void)_can.getStatus(_stats.can);

    _ad.rx_start = _stats.rx_frames;
    _ad.step_start_ms = millis();
    _ad.phase = 1;

    snprintf(_ad.last_msg, sizeof(_ad.last_msg), "Escutando %lu ms em %lu bps (%s)...",
             (unsigned long)_ad.dwell_ms,
             (unsigned long)_ad.current_bitrate,
             protoToStr(_ad.current_proto));
    return;
  }

  // Estado 1: escutando
  if (_ad.phase == 1) {
    const uint32_t now = millis();
    if ((now - _ad.step_start_ms) < _ad.dwell_ms) {
      return;
    }

    const uint32_t rxDelta = _stats.rx_frames - _ad.rx_start;
    if (rxDelta < _ad.min_frames) {
      snprintf(_ad.last_msg, sizeof(_ad.last_msg), "Sem tráfego (%lu frames) em %lu bps (%s).",
               (unsigned long)rxDelta,
               (unsigned long)_ad.current_bitrate,
               protoToStr(_ad.current_proto));
      _ad.step++;
      _ad.phase = 0;
      return;
    }

    // Bitrate parece ativo => enviar probe OBD (um frame) no protocolo atual
    if (!_can.begin(_ad.current_bitrate, false)) {
      snprintf(_ad.last_msg, sizeof(_ad.last_msg), "Falha ao reiniciar CAN em modo NORMAL (%lu bps).", (unsigned long)_ad.current_bitrate);
      _ad.step++;
      _ad.phase = 0;
      return;
    }
    (void)_can.getStatus(_stats.can);

    CanFrame req;
    req.ts_us = (uint64_t)esp_timer_get_time();
    req.rtr = false;
    req.dlc = 8;
    memset(req.data, 0x00, sizeof(req.data));
    req.data[0] = 0x02;
    req.data[1] = 0x01;
    req.data[2] = 0x00; // PID 00

    if (_ad.current_proto == ObdPoller::Protocol::OBD11) {
      req.id = 0x7DF;
      req.ext = false;
    } else {
      req.id = 0x18DB33F1;
      req.ext = true;
    }

    (void)_can.send(req);
    if (_stats.sd_ok) {
      _sd.logCanFrame(req, 'T');
    }

    _ad.probe_start_ms = millis();
    _ad.probe_ts_us = req.ts_us;
    _ad.phase = 2;

    snprintf(_ad.last_msg, sizeof(_ad.last_msg), "Probe enviado em %lu bps (%s). Aguardando resposta...",
             (unsigned long)_ad.current_bitrate,
             protoToStr(_ad.current_proto));
    return;
  }

  // Estado 2: aguardando resposta ao probe
  if (_ad.phase == 2) {
    // Procura em ring buffer por resposta ISO-TP Single Frame 0x41 0x00
    CanFrame frames[64];
    const size_t n = _ring.copyLatest(frames, 64);

    bool got = false;
    for (size_t i = 0; i < n; i++) {
      const CanFrame& f = frames[i];
      if (f.ts_us <= _ad.probe_ts_us) continue;

      // Match por protocolo
      if (_ad.current_proto == ObdPoller::Protocol::OBD11) {
        if (f.ext) continue;
        if (f.id < 0x7E8 || f.id > 0x7EF) continue;
      } else {
        if (!f.ext) continue;
        // respostas típicas: 0x18DAF1xx
        if ((f.id & 0x1FFFFF00UL) != 0x18DAF100UL) continue;
      }

      // ISO-TP single frame: nibble alto 0x0
      if (f.dlc < 4) continue;
      if ((f.data[0] & 0xF0) != 0x00) continue;

      const uint8_t len = f.data[0] & 0x0F;
      if (len < 2) continue;
      // payload inicia em data[1]
      if (f.data[1] != 0x41) continue;
      if (f.data[2] != 0x00) continue;

      got = true;
      break;
    }

    if (got) {
      _ad.running = false;
      _ad.found = true;
      _ad.phase = 0;
      _ad.found_bitrate = _ad.current_bitrate;
      _ad.found_proto = _ad.current_proto;

      snprintf(_ad.last_msg, sizeof(_ad.last_msg), "Encontrado: %lu bps + %s.",
               (unsigned long)_ad.found_bitrate,
               protoToStr(_ad.found_proto));

      // Aplica config encontrada (NORMAL)
      (void)applyCanConfig(_ad.found_bitrate, false, _ad.found_proto);

      if (_stats.sd_ok) {
        char msg[128];
        snprintf(msg, sizeof(msg), "bitrate=%lu proto=%s",
                 (unsigned long)_ad.found_bitrate,
                 protoToStr(_ad.found_proto));
        _sd.logEvent((uint64_t)esp_timer_get_time(), "autodetect", msg);
      }
      return;
    }

    const uint32_t now = millis();
    if ((now - _ad.probe_start_ms) >= _ad.probe_ms) {
      snprintf(_ad.last_msg, sizeof(_ad.last_msg), "Sem resposta ao probe em %lu bps (%s).",
               (unsigned long)_ad.current_bitrate,
               protoToStr(_ad.current_proto));
      _ad.step++;
      _ad.phase = 0;
      return;
    }
  }
}

// -------------------- Frames API --------------------

void WebPortal::handleFrames() {
  // Retorna últimos frames como JSON (compatível com versão anterior)
  CanFrame frames[64];
  size_t n = _ring.copyLatest(frames, 64);

  String json;
  json.reserve(4096);
  json += "[";

  static const char HEXCHARS[] = "0123456789ABCDEF";

  for (size_t i = 0; i < n; i++) {
    const CanFrame& f = frames[i];

    if (i) json += ",";

    char tsbuf[24];
    snprintf(tsbuf, sizeof(tsbuf), "%llu", (unsigned long long)f.ts_us);

    char hexbuf[2 * 8 + 1];
    size_t k = 0;
    for (uint8_t j = 0; j < f.dlc && j < 8; j++) {
      uint8_t b = f.data[j];
      hexbuf[k++] = HEXCHARS[(b >> 4) & 0x0F];
      hexbuf[k++] = HEXCHARS[(b     ) & 0x0F];
    }
    hexbuf[k] = '\0';

    json += "{";
    json += "\"ts_us\":" + String(tsbuf) + ",";
    json += "\"id\":" + String((unsigned long)f.id) + ",";
    json += "\"ext\":" + String(f.ext ? "true" : "false") + ",";
    json += "\"rtr\":" + String(f.rtr ? "true" : "false") + ",";
    json += "\"dlc\":" + String((unsigned)f.dlc) + ",";
    json += "\"data_hex\":\"" + String(hexbuf) + "\"";
    json += "}";
  }

  json += "]";
  _srv.send(200, "application/json", json);
}

// -------------------- Mark endpoint --------------------

void WebPortal::handleMark() {
  if (!_stats.sd_ok) {
    _srv.send(500, "text/plain", "SD not available");
    return;
  }

  String tag = _srv.hasArg("tag") ? _srv.arg("tag") : String("mark");
  String msg = _srv.hasArg("msg") ? _srv.arg("msg") : String("");

  _sd.logEvent((uint64_t)esp_timer_get_time(), tag.c_str(), msg.c_str());
  _srv.send(200, "text/plain", "ok");
}

// -------------------- SD UI / Download / Manage --------------------

void WebPortal::handleFiles() {
  if (!_stats.sd_ok) {
    _srv.send(200, "text/html", htmlHeader("Files") + "<h2>SD</h2><p class='err'>SD não disponível.</p>" + htmlFooter());
    return;
  }

  // Diretório atual
  String dir = _srv.hasArg("dir") ? _srv.arg("dir") : String(AppConfig::LOG_DIR);
  dir = normalizePath(dir);
  if (!isSafePath(dir)) dir = String(AppConfig::LOG_DIR);

  String html = htmlHeader("Files");
  html += "<h2>Arquivos no SD</h2>";
  html += "<p><a href='/'>Voltar</a> | <a href='/config'>Config</a></p>";

  if (_srv.hasArg("msg")) {
    html += "<p class='pill'>" + _srv.arg("msg") + "</p>";
  }

  html += "<p>Diretório: <code>" + dir + "</code></p>";

  // Links de navegação
  html += "<p>";
  if (dir != "/") {
    // calcula pai
    String parent = dir;
    int slash = parent.lastIndexOf('/');
    if (slash <= 0) parent = "/";
    else parent = parent.substring(0, slash);
    html += "<a href='/files?dir=" + urlEncode(parent) + "'>⬅ Pasta acima</a> | ";
  }
  html += "<a href='/files?dir=/'>Raiz</a> | <a href='/files?dir=" + urlEncode(String(AppConfig::LOG_DIR)) + "'>/logs</a>";
  html += "</p>";

  // Mostra quais arquivos estão abertos (protegidos)
  html += "<h3>Arquivos protegidos (abertos nesta sessão)</h3>";
  html += "<ul>";
  html += "<li>RAW: " + _sd.rawPath() + "</li>";
  html += "<li>OBD: " + _sd.obdPath() + "</li>";
  html += "<li>UDS: " + _sd.udsPath() + "</li>";
  html += "<li>STATS: " + _sd.statsPath() + "</li>";
  html += "<li>EVENTS: " + _sd.eventsPath() + "</li>";
  html += "</ul>";

  // Lista de arquivos
  File root = SD.open(dir);
  if (!root || !root.isDirectory()) {
    html += "<p class='err'>Erro ao abrir diretório.</p>";
    html += htmlFooter();
    _srv.send(200, "text/html", html);
    return;
  }

  html += "<h3>Conteúdo</h3>";
  html += "<table><tr><th>Nome</th><th>Tamanho</th><th>Ações</th></tr>";

  File f = root.openNextFile();
  while (f) {
    String name = normalizePath(String(f.name()));

    if (f.isDirectory()) {
      html += "<tr><td><b>DIR</b> <a href='/files?dir=" + urlEncode(name) + "'>" + name + "</a></td>";
      html += "<td>-</td>";
      html += "<td>";
      // mkdir/rmdir não implementado para diretórios aqui
      html += "</td></tr>";
      f.close();
      f = root.openNextFile();
      continue;
    }

    const bool locked = isProtectedPath(name);

    html += "<tr>";
    html += "<td><a href='/download?name=" + urlEncode(name) + "'>" + name + "</a></td>";
    html += "<td>" + String((unsigned long)f.size()) + "</td>";

    html += "<td>";
    if (locked) {
      html += "<span class='warn'>PROTECTED</span>";
    } else {
      html += "<a href='/delete?name=" + urlEncode(name) + "&dir=" + urlEncode(dir) + "' onclick=\"return confirm('Apagar?');\">Apagar</a>";
      html += "&nbsp;|&nbsp;";
      // rename: formulário simples
      html += "<form style='display:inline' method='GET' action='/rename'>";
      html += "<input type='hidden' name='from' value='" + name + "'>";
      html += "<input type='hidden' name='dir' value='" + dir + "'>";
      html += "<input name='to' placeholder='novo_nome' size='12'>";
      html += "<input type='submit' value='Renomear'>";
      html += "</form>";
    }
    html += "</td>";
    html += "</tr>";

    f.close();
    f = root.openNextFile();
  }

  html += "</table>";
  root.close();

  // Upload
  html += "<h3>Upload para o SD</h3>";
  html += "<form method='POST' action='/upload?dir=" + urlEncode(dir) + "' enctype='multipart/form-data'>";
  html += "<input type='file' name='file'>";
  html += "<input type='submit' value='Enviar'>";
  html += "</form>";

  // Mkdir
  html += "<h3>Criar diretório</h3>";
  html += "<form method='GET' action='/mkdir'>";
  html += "<input type='hidden' name='base' value='" + dir + "'>";
  html += "<label>Nome: <input name='name' placeholder='nova_pasta'></label>";
  html += "<input type='submit' value='Criar'>";
  html += "</form>";

  html += htmlFooter();
  _srv.send(200, "text/html", html);
}

void WebPortal::handleDownload() {
  if (!_stats.sd_ok) {
    _srv.send(500, "text/plain", "SD not available");
    return;
  }
  if (!_srv.hasArg("name")) {
    _srv.send(400, "text/plain", "missing ?name=");
    return;
  }

  String name = normalizePath(_srv.arg("name"));
  if (!isSafePath(name)) {
    _srv.send(400, "text/plain", "invalid path");
    return;
  }

  // Content-Type simples
  String contentType = "application/octet-stream";
  if (name.endsWith(".csv")) contentType = "text/csv";
  if (name.endsWith(".dbc")) contentType = "text/plain";

  File f = SD.open(name, FILE_READ);
  if (!f) {
    _srv.send(404, "text/plain", "file not found");
    return;
  }

  // Força download
  String filename = name;
  int slash = filename.lastIndexOf('/');
  if (slash >= 0) filename = filename.substring(slash + 1);
  _srv.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");

  _srv.streamFile(f, contentType);
  f.close();
}

void WebPortal::handleDelete() {
  if (!_stats.sd_ok) {
    _srv.send(500, "text/plain", "SD not available");
    return;
  }

  if (!_srv.hasArg("name")) {
    _srv.send(400, "text/plain", "missing ?name=");
    return;
  }

  String name = normalizePath(_srv.arg("name"));
  String backDir = _srv.hasArg("dir") ? normalizePath(_srv.arg("dir")) : String(AppConfig::LOG_DIR);

  if (!isSafePath(name)) {
    _srv.send(400, "text/plain", "invalid path");
    return;
  }

  if (isProtectedPath(name)) {
    _srv.sendHeader("Location", "/files?dir=" + urlEncode(backDir) + "&msg=Arquivo%20protegido", true);
    _srv.send(302, "text/plain", "");
    return;
  }

  bool ok = false;
  File f = SD.open(name);
  if (f) {
    bool isDir = f.isDirectory();
    f.close();

    if (isDir) {
      // Remoção de diretórios (rmdir) varia por implementação; mantemos simples.
      ok = false;
    } else {
      ok = SD.remove(name);
    }
  }

  _srv.sendHeader("Location", "/files?dir=" + urlEncode(backDir) + "&msg=" + String(ok ? "Apagado" : "Falha%20ao%20apagar"), true);
  _srv.send(302, "text/plain", "");
}

void WebPortal::handleRename() {
  if (!_stats.sd_ok) {
    _srv.send(500, "text/plain", "SD not available");
    return;
  }

  if (!_srv.hasArg("from") || !_srv.hasArg("to")) {
    _srv.send(400, "text/plain", "missing ?from= and ?to=");
    return;
  }

  String from = normalizePath(_srv.arg("from"));
  String to = _srv.arg("to");
  String backDir = _srv.hasArg("dir") ? normalizePath(_srv.arg("dir")) : String(AppConfig::LOG_DIR);

  if (!isSafePath(from)) {
    _srv.send(400, "text/plain", "invalid from path");
    return;
  }

  if (isProtectedPath(from)) {
    _srv.sendHeader("Location", "/files?dir=" + urlEncode(backDir) + "&msg=Arquivo%20protegido", true);
    _srv.send(302, "text/plain", "");
    return;
  }

  // Se 'to' não for caminho absoluto, renomeia dentro do mesmo diretório de 'from'
  to.trim();
  if (to.length() == 0) {
    _srv.sendHeader("Location", "/files?dir=" + urlEncode(backDir) + "&msg=Nome%20invalido", true);
    _srv.send(302, "text/plain", "");
    return;
  }

  // remove qualquer path do browser (C:\fakepath\...)
  to.replace('\\', '/');
  int slash = to.lastIndexOf('/');
  if (slash >= 0) {
    to = to.substring(slash + 1);
  }

  String dest;
  if (to.startsWith("/")) {
    dest = normalizePath(to);
  } else {
    int lastSlash = from.lastIndexOf('/');
    String base = (lastSlash <= 0) ? String("/") : from.substring(0, lastSlash);
    if (base.length() == 0) base = "/";
    dest = normalizePath(base + "/" + to);
  }

  if (!isSafePath(dest)) {
    _srv.sendHeader("Location", "/files?dir=" + urlEncode(backDir) + "&msg=Destino%20invalido", true);
    _srv.send(302, "text/plain", "");
    return;
  }

  if (isProtectedPath(dest)) {
    _srv.sendHeader("Location", "/files?dir=" + urlEncode(backDir) + "&msg=Destino%20protegido", true);
    _srv.send(302, "text/plain", "");
    return;
  }

  bool ok = SD.rename(from, dest);
  _srv.sendHeader("Location", "/files?dir=" + urlEncode(backDir) + "&msg=" + String(ok ? "Renomeado" : "Falha%20ao%20renomear"), true);
  _srv.send(302, "text/plain", "");
}

void WebPortal::handleMkdir() {
  if (!_stats.sd_ok) {
    _srv.send(500, "text/plain", "SD not available");
    return;
  }

  String base = _srv.hasArg("base") ? normalizePath(_srv.arg("base")) : String("/");
  String name = _srv.hasArg("name") ? _srv.arg("name") : String("");
  name.trim();
  if (name.length() == 0) {
    _srv.sendHeader("Location", "/files?dir=" + urlEncode(base) + "&msg=Nome%20invalido", true);
    _srv.send(302, "text/plain", "");
    return;
  }

  // sanitiza
  name.replace('\\', '/');
  int slash = name.lastIndexOf('/');
  if (slash >= 0) {
    name = name.substring(slash + 1);
  }

  String dir = normalizePath(base + "/" + name);
  if (!isSafePath(dir)) {
    _srv.sendHeader("Location", "/files?dir=" + urlEncode(base) + "&msg=Destino%20invalido", true);
    _srv.send(302, "text/plain", "");
    return;
  }

  bool ok = SD.mkdir(dir);
  _srv.sendHeader("Location", "/files?dir=" + urlEncode(base) + "&msg=" + String(ok ? "Diretorio%20criado" : "Falha%20ao%20criar"), true);
  _srv.send(302, "text/plain", "");
}

void WebPortal::handleUploadPost() {
  String dir = _srv.hasArg("dir") ? normalizePath(_srv.arg("dir")) : String(AppConfig::LOG_DIR);
  _srv.sendHeader("Location", "/files?dir=" + urlEncode(dir) + "&msg=Upload%20finalizado", true);
  _srv.send(302, "text/plain", "");
}

void WebPortal::handleUpload() {
  if (!_stats.sd_ok) return;

  HTTPUpload& upload = _srv.upload();
  if (upload.status == UPLOAD_FILE_START) {
    String dir = _srv.hasArg("dir") ? normalizePath(_srv.arg("dir")) : String(AppConfig::LOG_DIR);
    if (!isSafePath(dir)) dir = String(AppConfig::LOG_DIR);

    String filename = upload.filename;
    filename.replace('\\', '/');
    int slash = filename.lastIndexOf('/');
    if (slash >= 0) filename = filename.substring(slash + 1);

    if (filename.length() == 0) {
      _uploadPath = "";
      return;
    }

    _uploadPath = normalizePath(dir + "/" + filename);
    if (!isSafePath(_uploadPath) || isProtectedPath(_uploadPath)) {
      _uploadPath = "";
      return;
    }

    // Se existir, remove primeiro para evitar append
    if (SD.exists(_uploadPath)) {
      SD.remove(_uploadPath);
    }

    _uploadFile = SD.open(_uploadPath, FILE_WRITE);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (_uploadFile) {
      _uploadFile.write(upload.buf, upload.currentSize);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (_uploadFile) {
      _uploadFile.close();
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (_uploadFile) {
      _uploadFile.close();
    }
    if (_uploadPath.length() && SD.exists(_uploadPath)) {
      SD.remove(_uploadPath);
    }
  }
}

void WebPortal::handleNotFound() {
  // redireciona para root (ajuda captive portal)
  _srv.sendHeader("Location", String("http://") + AP_IP.toString() + "/", true);
  _srv.send(302, "text/plain", "");
}
