#pragma once
#include <Arduino.h>
#include "can_twai.h"
#include "sd_logger.h"

// Poller simples para OBD-II (ISO 15765-4 CAN 11-bit) e UDS (0x22) em CAN.
// - Envia requisições periodicamente (sem ELM327)
// - Decodifica alguns PIDs comuns (Mode 01) quando possível
// - Loga tudo no SD (raw + valores decodificados)

class ObdPoller {
public:
  enum class Protocol : uint8_t {
    RAW  = 0,   // não envia diagnósticos (apenas log RX/TX manual)
    OBD11 = 1,  // ISO 15765-4 CAN 11-bit (0x7DF / 0x7E8..0x7EF)
    OBD29 = 2,  // ISO 15765-4 CAN 29-bit (0x18DB33F1 / 0x18DAF1xx)
  };

  ObdPoller(CanTwai& can, SdLogger& sd, RuntimeStats* stats = nullptr);

  void begin();
  void loop();
  void onRxFrame(const CanFrame& f);

  // Reconfiguração em runtime (usado pelo portal web)
  void reset();
  void setProtocol(Protocol p);
  Protocol protocol() const { return _proto; }

  void setObdEnabled(bool en) { _obdEnabled = en; }
  bool obdEnabled() const { return _obdEnabled; }

  void setUdsEnabled(bool en) { _udsEnabled = en; }
  bool udsEnabled() const { return _udsEnabled; }

  void setUdsConfig(uint32_t req_id, uint32_t resp_id, uint16_t did, bool ext = false);

  // Getters para UI
  uint32_t obdReqId() const { return _obdReqId; }
  bool obdExt() const { return _obdExt; }
  uint32_t udsReqId() const { return _udsReqId; }
  uint32_t udsRespId() const { return _udsRespId; }
  uint16_t udsDid() const { return _udsDid; }
  bool udsExt() const { return _udsExt; }

private:
  CanTwai& _can;
  SdLogger& _sd;
  RuntimeStats* _stats; // opcional (para contadores TX)

  Protocol _proto = Protocol::OBD11;

  // OBD / ISO-TP
  bool _obdEnabled = true;
  bool _obdExt = false;
  uint32_t _obdReqId = 0x7DF;
  uint32_t _obdRespMin = 0x7E8;
  uint32_t _obdRespMax = 0x7EF;
  uint32_t _obdRespMask = 0;
  uint32_t _obdRespValue = 0;

  // --- OBD polling ---
  struct PidDef {
    uint8_t pid;
    const char* name;
    const char* unit;
  };

  static constexpr uint32_t OBD11_REQ_ID   = 0x7DF;
  static constexpr uint32_t OBD11_RESP_MIN = 0x7E8;
  static constexpr uint32_t OBD11_RESP_MAX = 0x7EF;

  // 29-bit OBD-II (funcional) – request 0x18DB33F1, respostas típicas 0x18DAF1xx
  static constexpr uint32_t OBD29_REQ_ID    = 0x18DB33F1;
  static constexpr uint32_t OBD29_RESP_MASK = 0x1FFFFF00; // ignora o último byte (ECU source address)
  static constexpr uint32_t OBD29_RESP_VALUE = 0x18DAF100;

  static constexpr size_t kPidCount = 12;
  static const PidDef kPids[kPidCount];

  bool _pidSupported[256] = {false};
  bool _pidSupportKnown = false;     // terminou fase inicial (00/20/40/60)
  bool _gotSupportBitmap = false;    // recebeu ao menos uma resposta válida de bitmask

  uint32_t _lastReqMs = 0;
  size_t _pidIndex = 0;

  // --- UDS custom polling (service 0x22) ---
  bool _udsEnabled = false;
  uint32_t _lastUdsReqMs = 0;

  bool _udsExt = false;
  uint32_t _udsReqId = 0x78A;
  uint32_t _udsRespId = 0x7CA;
  uint16_t _udsDid = 0xD001;

  static constexpr uint32_t UDS_REQ_ID_DEFAULT  = 0x78A;
  static constexpr uint32_t UDS_RESP_ID_DEFAULT = 0x7CA;
  static constexpr uint16_t UDS_DID_DEFAULT     = 0xD001;

  // helpers
  void sendObdPid(uint8_t pid);
  void sendUdsDid(uint16_t did);

  bool matchObdResponse(const CanFrame& f) const;
  bool matchUdsResponse(const CanFrame& f) const;

  static bool parseIsoTpSingleFrame(const CanFrame& f, uint8_t* payload, uint8_t& payloadLen);
  void handleObdResponse(const CanFrame& f, const uint8_t* payload, uint8_t payloadLen);
  void handleUdsResponse(const CanFrame& f, const uint8_t* payload, uint8_t payloadLen);

  void markSupportedPids(uint8_t basePid, const uint8_t* A_B_C_D);
  bool isPidSupported(uint8_t pid) const;
  const PidDef* findPid(uint8_t pid) const;
};
