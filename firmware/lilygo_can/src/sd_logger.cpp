#include "sd_logger.h"
#include "app_config.h"
#include "pin_config.h"

static const char HEXCHARS[] = "0123456789ABCDEF";

SdLogger::SdLogger() {}

void SdLogger::ensureDir(const char* dir) {
  if (!SD.exists(dir)) {
    SD.mkdir(dir);
  }
}

String SdLogger::nextFileName(const char* prefix, const char* ext) {
  // Procura o próximo índice livre (000..999)
  for (int i = 0; i < 1000; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s/%s_%03d.%s", AppConfig::LOG_DIR, prefix, i, ext);
    if (!SD.exists(buf)) {
      return String(buf);
    }
  }
  // fallback (pouco provável)
  char buf[64];
  snprintf(buf, sizeof(buf), "%s/%s_overflow.%s", AppConfig::LOG_DIR, prefix, ext);
  return String(buf);
}

bool SdLogger::begin() {
  // SPI para SD com pinos do T-CAN485
  _spi.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS, _spi)) {
    _ok = false;
    return false;
  }

  ensureDir(AppConfig::LOG_DIR);

  // Arquivo RAW
  String rawName = nextFileName("canlog", "csv");
  _rawPath = rawName;
  _raw = SD.open(rawName, FILE_WRITE);
  if (!_raw) {
    _ok = false;
    return false;
  }
  _raw.println("ts_us,can_id,is_ext,is_rtr,dlc,data_hex,dir");

  // Arquivo OBD decodificado (quando possível)
  String obdName = nextFileName("obd", "csv");
  _obdPath = obdName;
  _obd = SD.open(obdName, FILE_WRITE);
  if (_obd) {
    _obd.println("ts_us,src_id,service,pid_or_did,name,value,unit");
  }

  // Arquivo UDS raw (service 0x22 etc)
  String udsName = nextFileName("uds", "csv");
  _udsPath = udsName;
  _uds = SD.open(udsName, FILE_WRITE);
  if (_uds) {
    _uds.println("ts_us,src_id,did,data_hex");
  }

  // Arquivo de estatísticas (1 linha por segundo)
  String statsName = nextFileName("stats", "csv");
  _statsPath = statsName;
  _stats = SD.open(statsName, FILE_WRITE);
  if (_stats) {
    _stats.println("uptime_ms,rx_frames,tx_frames,fps,can_state,bus_err,rx_missed,rx_overrun,sd_ok");
  }

  // Arquivo de eventos / marcações (útil para delimitar fases de experimento)
  String eventsName = nextFileName("events", "csv");
  _eventsPath = eventsName;
  _events = SD.open(eventsName, FILE_WRITE);
  if (_events) {
    _events.println("ts_us,tag,msg");
  }

  _ok = true;
  _lastFlushMs = millis();
  _lastStatsLogMs = 0;
  flushAll();
  return true;
}

void SdLogger::flushAll() {
  if (_raw) _raw.flush();
  if (_obd) _obd.flush();
  if (_uds) _uds.flush();
  if (_stats) _stats.flush();
  if (_events) _events.flush();
}

void SdLogger::flush() {
  if (!_ok) return;
  flushAll();
}

void SdLogger::loop() {
  if (!_ok) return;
  uint32_t now = millis();
  if (now - _lastFlushMs >= AppConfig::LOG_FLUSH_MS) {
    _lastFlushMs = now;
    flushAll();
  }
}

void SdLogger::logCanFrame(const CanFrame& f, char dir) {
  if (!_ok || !_raw) return;

  // data_hex (2*dlc)
  char hexbuf[2 * 8 + 1];
  size_t n = 0;
  for (uint8_t i = 0; i < f.dlc && i < 8; i++) {
    uint8_t b = f.data[i];
    hexbuf[n++] = HEXCHARS[(b >> 4) & 0x0F];
    hexbuf[n++] = HEXCHARS[(b     ) & 0x0F];
  }
  hexbuf[n] = '\0';

  _raw.print((unsigned long long)f.ts_us);
  _raw.print(',');
  _raw.print((unsigned long)f.id);
  _raw.print(',');
  _raw.print(f.ext ? 1 : 0);
  _raw.print(',');
  _raw.print(f.rtr ? 1 : 0);
  _raw.print(',');
  _raw.print((unsigned)f.dlc);
  _raw.print(',');
  _raw.print(hexbuf);
  _raw.print(',');
  _raw.println(dir);

  if (writeFailed()) {
    _ok = false;
  }
}

void SdLogger::logObdValue(uint64_t ts_us, uint32_t src_id,
                           uint8_t service, uint16_t pid_or_did,
                           const char* name, float value, const char* unit) {
  if (!_ok || !_obd) return;

  _obd.print((unsigned long long)ts_us);
  _obd.print(',');
  _obd.print((unsigned long)src_id);
  _obd.print(',');
  _obd.print((unsigned)service);
  _obd.print(',');
  _obd.print((unsigned)pid_or_did);
  _obd.print(',');
  _obd.print(name ? name : "");
  _obd.print(',');
  _obd.print(value, 3);
  _obd.print(',');
  _obd.println(unit ? unit : "");

  if (writeFailed()) {
    _ok = false;
  }
}

void SdLogger::logUdsRaw(uint64_t ts_us, uint32_t src_id, uint16_t did, const uint8_t* payload, size_t len) {
  if (!_ok || !_uds) return;

  char hexbuf[2 * 64 + 1];
  size_t n = 0;
  size_t maxlen = len;
  if (maxlen > 64) maxlen = 64;
  for (size_t i = 0; i < maxlen; i++) {
    uint8_t b = payload[i];
    hexbuf[n++] = HEXCHARS[(b >> 4) & 0x0F];
    hexbuf[n++] = HEXCHARS[(b     ) & 0x0F];
  }
  hexbuf[n] = '\0';

  _uds.print((unsigned long long)ts_us);
  _uds.print(',');
  _uds.print((unsigned long)src_id);
  _uds.print(',');
  _uds.print((unsigned)did);
  _uds.print(',');
  _uds.println(hexbuf);

  if (writeFailed()) {
    _ok = false;
  }
}

void SdLogger::logRuntimeStats(uint32_t uptime_ms, const RuntimeStats& st) {
  if (!_ok || !_stats) return;

  // Evita spam caso seja chamado mais de 1x por segundo.
  if (_lastStatsLogMs != 0 && (uptime_ms - _lastStatsLogMs) < 900) {
    return;
  }
  _lastStatsLogMs = uptime_ms;

  _stats.print(uptime_ms);
  _stats.print(',');
  _stats.print(st.rx_frames);
  _stats.print(',');
  _stats.print(st.tx_frames);
  _stats.print(',');
  _stats.print(st.fps);
  _stats.print(',');
  _stats.print(st.can.state);
  _stats.print(',');
  _stats.print(st.can.bus_error_count);
  _stats.print(',');
  _stats.print(st.can.rx_missed_count);
  _stats.print(',');
  _stats.print(st.can.rx_overrun_count);
  _stats.print(',');
  _stats.println(st.sd_ok ? 1 : 0);

  if (writeFailed()) {
    _ok = false;
  }
}

void SdLogger::logEvent(uint64_t ts_us, const char* tag, const char* msg) {
  if (!_ok || !_events) return;

  _events.print((unsigned long long)ts_us);
  _events.print(',');
  _events.print(tag ? tag : "");
  _events.print(',');
  _events.println(msg ? msg : "");

  if (writeFailed()) {
    _ok = false;
  }
}

void SdLogger::listFilesHtml(String& out, const char* dir) const {
  File root = SD.open(dir);
  if (!root) {
    out += "<p>Erro ao abrir dir.</p>";
    return;
  }
  if (!root.isDirectory()) {
    out += "<p>Não é diretório.</p>";
    root.close();
    return;
  }

  out += "<ul>";
  File f = root.openNextFile();
  while (f) {
    String name = String(f.name());
    if (f.isDirectory()) {
      out += "<li><b>DIR</b> ";
      out += name;
      out += "</li>";
    } else {
      out += "<li><a href=\"/download?name=";
      // O endpoint /download aceita caminhos com ou sem '/'.
      out += name;
      out += "\">";
      out += name;
      out += "</a> (";
      out += String((unsigned long)f.size());
      out += " bytes)</li>";
    }
    f.close();
    f = root.openNextFile();
  }
  out += "</ul>";
  root.close();
}

bool SdLogger::streamFileToClient(const String& path, Stream& client) {
  if (!_ok) return false;
  File f = SD.open(path, FILE_READ);
  if (!f) return false;

  const size_t bufSize = 1024;
  uint8_t buf[bufSize];
  while (true) {
    int n = f.read(buf, bufSize);
    if (n <= 0) break;
    client.write(buf, n);
  }
  f.close();
  return true;
}

String SdLogger::listFilesHtml() const {
  if (!_ok) {
    return "<p>SD nao inicializado.</p>";
  }
  String html;
  listFilesHtml(html, "/");
  return html;
}

bool SdLogger::writeFailed() {
  // File herda de Print, que mantém um flag de erro de escrita.
  // Além disso, se o File foi fechado por alguma razão, ele avalia como false.
  if (_raw && _raw.getWriteError()) return true;
  if (_obd && _obd.getWriteError()) return true;
  if (_uds && _uds.getWriteError()) return true;
  if (_stats && _stats.getWriteError()) return true;
  if (_events && _events.getWriteError()) return true;
  return false;
}
