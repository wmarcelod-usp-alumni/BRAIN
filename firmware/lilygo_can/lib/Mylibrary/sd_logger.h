#pragma once
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include "types.h"

class SdLogger {
public:
  SdLogger();

  bool begin();         // monta SD e abre arquivos
  void loop();          // flush periódico

  bool ok() const { return _ok; }

  // Força flush imediato (útil antes de download via Wi‑Fi ou em checkpoints de experimento).
  void flush();

  void logCanFrame(const CanFrame& f, char dir); // dir: 'R' (rx) / 'T' (tx)
  void logObdValue(uint64_t ts_us, uint32_t src_id,
                   uint8_t service, uint16_t pid_or_did,
                   const char* name, float value, const char* unit);

  void logUdsRaw(uint64_t ts_us, uint32_t src_id, uint16_t did, const uint8_t* payload, size_t len);

  // Log de estatísticas (1x por segundo) para auditoria offline.
  void logRuntimeStats(uint32_t uptime_ms, const RuntimeStats& st);

  // Log de eventos / marcações (ex.: "start", "attack_on", "attack_off").
  void logEvent(uint64_t ts_us, const char* tag, const char* msg);

  // Web portal helpers
  void listFilesHtml(String& out, const char* dir = "/") const;
  bool streamFileToClient(const String& path, Stream& client);
  String listFilesHtml() const;

  // Caminhos dos logs ativos (útil para UI e para evitar exclusão acidental)
  const String& rawPath() const { return _rawPath; }
  const String& obdPath() const { return _obdPath; }
  const String& udsPath() const { return _udsPath; }
  const String& statsPath() const { return _statsPath; }
  const String& eventsPath() const { return _eventsPath; }

private:
  bool _ok = false;
  SPIClass _spi = SPIClass(VSPI);

  File _raw;
  File _obd;
  File _uds;

  // Logs auxiliares
  File _stats;
  File _events;

  // Nomes completos (com diretório) dos arquivos abertos nesta sessão
  String _rawPath;
  String _obdPath;
  String _udsPath;
  String _statsPath;
  String _eventsPath;

  uint32_t _lastFlushMs = 0;
  uint32_t _lastStatsLogMs = 0;

  static String nextFileName(const char* prefix, const char* ext);
  static void ensureDir(const char* dir);

  void flushAll();

  // Nota: a API Arduino Print::getWriteError() não é 'const'.
  // Portanto, esta função também não pode ser const.
  bool writeFailed(); // ajuda a detectar falha de escrita
};
