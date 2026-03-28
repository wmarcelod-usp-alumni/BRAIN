#include "obd_poller.h"
#include <esp_timer.h>
#include "app_config.h"


const ObdPoller::PidDef ObdPoller::kPids[ObdPoller::kPidCount] = {
  {0x00, "Supported PIDs [01-20]", ""},   // bitmask
  {0x20, "Supported PIDs [21-40]", ""},   // bitmask
  {0x40, "Supported PIDs [41-60]", ""},   // bitmask
  {0x60, "Supported PIDs [61-80]", ""},   // bitmask

  {0x0C, "Engine RPM", "rpm"},
  {0x0D, "Vehicle speed", "km/h"},
  {0x11, "Throttle position", "%"},
  {0x42, "Control module voltage", "V"},
  {0x5A, "Relative accelerator pedal position", "%"},
  {0x5B, "Hybrid/EV battery pack remaining charge", "%"},
  {0x51, "Fuel type", "enum"},
  {0x1F, "Time since engine start", "s"},
};


ObdPoller::ObdPoller(CanTwai& can, SdLogger& sd, RuntimeStats* stats) : _can(can), _sd(sd), _stats(stats) {}

void ObdPoller::begin() {
  // Defaults (podem ser sobrescritos via portal web em runtime)
  setProtocol(Protocol::OBD11);
  _obdEnabled = true;

  _udsEnabled = AppConfig::UDS_DEFAULT_ENABLED;
  setUdsConfig(UDS_REQ_ID_DEFAULT, UDS_RESP_ID_DEFAULT, UDS_DID_DEFAULT, false);

  reset();
}

void ObdPoller::reset() {
  // Fase inicial: perguntar PIDs suportados (00/20/40/60)
  _pidSupportKnown = false;
  _gotSupportBitmap = false;
  memset(_pidSupported, 0, sizeof(_pidSupported));

  // Permite sempre os próprios "support PIDs" (para consulta)
  _pidSupported[0x00] = true;
  _pidSupported[0x20] = true;
  _pidSupported[0x40] = true;
  _pidSupported[0x60] = true;

  _pidIndex = 0;
  _lastReqMs = 0;
  _lastUdsReqMs = 0;
}

void ObdPoller::setProtocol(Protocol p) {
  _proto = p;
  switch (p) {
    case Protocol::RAW:
      _obdEnabled = false;
      // Nota: UDS permanece configurável separadamente. Se desejar "sniff" puro,
      // desabilite também _udsEnabled via setUdsEnabled(false).
      break;

    case Protocol::OBD11:
      _obdEnabled = true;
      _obdExt = false;
      _obdReqId = OBD11_REQ_ID;
      _obdRespMin = OBD11_RESP_MIN;
      _obdRespMax = OBD11_RESP_MAX;
      _obdRespMask = 0;
      _obdRespValue = 0;
      break;

    case Protocol::OBD29:
      _obdEnabled = true;
      _obdExt = true;
      _obdReqId = OBD29_REQ_ID;
      _obdRespMin = 0;
      _obdRespMax = 0;
      _obdRespMask = OBD29_RESP_MASK;
      _obdRespValue = OBD29_RESP_VALUE;
      break;
  }
}

void ObdPoller::setUdsConfig(uint32_t req_id, uint32_t resp_id, uint16_t did, bool ext) {
  _udsReqId = req_id;
  _udsRespId = resp_id;
  _udsDid = did;
  _udsExt = ext;
}

bool ObdPoller::matchObdResponse(const CanFrame& f) const {
  if (!_obdEnabled) return false;
  if (f.ext != _obdExt) return false;
  if (!_obdExt) {
    return (f.id >= _obdRespMin && f.id <= _obdRespMax);
  }
  return ((f.id & _obdRespMask) == _obdRespValue);
}

bool ObdPoller::matchUdsResponse(const CanFrame& f) const {
  if (!_udsEnabled) return false;
  if (f.ext != _udsExt) return false;
  return (f.id == _udsRespId);
}

const ObdPoller::PidDef* ObdPoller::findPid(uint8_t pid) const {
  for (size_t i = 0; i < kPidCount; i++) {
    if (kPids[i].pid == pid) return &kPids[i];
  }
  return nullptr;
}

bool ObdPoller::parseIsoTpSingleFrame(const CanFrame& f, uint8_t* payload, uint8_t& payloadLen) {
  if (f.dlc < 2) return false;

  const uint8_t pci = f.data[0];
  const uint8_t frameType = (pci >> 4) & 0x0F;
  if (frameType != 0x0) return false; // só Single Frame

  uint8_t len = pci & 0x0F;
  if (len == 0) return false;
  if (len > 7) return false; // em Single Frame clássico

  // Garante que não ultrapasse o que veio no CAN
  uint8_t maxPayload = (f.dlc - 1);
  if (len > maxPayload) len = maxPayload;

  memcpy(payload, &f.data[1], len);
  payloadLen = len;
  return true;
}

void ObdPoller::markSupportedPids(uint8_t basePid, const uint8_t* A_B_C_D) {
  // Bits A7..D0 => PIDs (base+1 .. base+0x20)
  for (int i = 0; i < 32; i++) {
    int byteIndex = i / 8;
    int bitIndex = 7 - (i % 8);
    bool supported = (A_B_C_D[byteIndex] >> bitIndex) & 0x01;
    if (supported) {
      uint8_t pid = (uint8_t)(basePid + 1 + i);
      _pidSupported[pid] = true;
    }
  }
}

bool ObdPoller::isPidSupported(uint8_t pid) const {
  if (!_pidSupportKnown) return true;         // ainda descobrindo => não bloquear
  if (!_gotSupportBitmap) return true;        // não conseguimos bitmask => não bloquear
  return _pidSupported[pid];
}

void ObdPoller::sendObdPid(uint8_t pid) {
  if (!_obdEnabled) return;
  CanFrame req;
  req.ts_us = (uint64_t)esp_timer_get_time();
  req.id = _obdReqId;
  req.ext = _obdExt;
  req.rtr = false;
  req.dlc = 8;
  memset(req.data, 0x00, sizeof(req.data));
  req.data[0] = 0x02;   // ISO-TP Single Frame length = 2 bytes
  req.data[1] = 0x01;   // OBD Service 01 (Current Data)
  req.data[2] = pid;

  if (_can.send(req)) {
    if (_stats) _stats->tx_frames++;
    _sd.logCanFrame(req, 'T');
  }
}

void ObdPoller::sendUdsDid(uint16_t did) {
  CanFrame req;
  req.ts_us = (uint64_t)esp_timer_get_time();
  req.id = _udsReqId;
  req.ext = _udsExt;
  req.rtr = false;
  req.dlc = 8;
  memset(req.data, 0x00, sizeof(req.data));
  req.data[0] = 0x03;   // length = 3 bytes (0x22 + DID hi + DID lo)
  req.data[1] = 0x22;   // UDS: ReadDataByIdentifier
  req.data[2] = (uint8_t)((did >> 8) & 0xFF);
  req.data[3] = (uint8_t)(did & 0xFF);

  if (_can.send(req)) {
    if (_stats) _stats->tx_frames++;
    _sd.logCanFrame(req, 'T');
  }
}

void ObdPoller::loop() {
  const uint32_t now = millis();

  // Se não há nenhum modo de diagnóstico ativo, sai rápido.
  if (!_obdEnabled && !_udsEnabled) {
    return;
  }

  // 1) UDS (opcional) — prioriza porque sabemos que pelo menos 0xD001 gerou resposta no seu log
  if (_udsEnabled && (now - _lastUdsReqMs >= AppConfig::UDS_REQ_INTERVAL_MS)) {
    _lastUdsReqMs = now;
    sendUdsDid(_udsDid);
    // evita mandar OBD no mesmo instante (reduz colisão)
    return;
  }

  if (!_obdEnabled) {
    return;
  }

  // 2) OBD Mode 01 — um PID por intervalo
  if (now - _lastReqMs < AppConfig::OBD_REQ_INTERVAL_MS) {
    return;
  }
  _lastReqMs = now;

  const size_t total = kPidCount;

  // Fase inicial: mandar 00,20,40,60 e depois liberar os demais
  if (!_pidSupportKnown) {
    if (_pidIndex < 4) {
      sendObdPid(kPids[_pidIndex].pid);
      _pidIndex++;
      return;
    }
    // terminou fase inicial
    _pidSupportKnown = true;
    if (_pidIndex < 4) _pidIndex = 4;
  }

  // Fase normal: varre lista e pula PIDs não suportados (se já conhecido)
  for (size_t tries = 0; tries < total; tries++) {
    if (_pidIndex >= total) _pidIndex = 4; // pula os 4 primeiros (suporte)

    uint8_t pid = kPids[_pidIndex].pid;
    const PidDef* def = &kPids[_pidIndex];
    _pidIndex++;

    // Não repete as queries de suporte aqui
    if (pid == 0x00 || pid == 0x20 || pid == 0x40 || pid == 0x60) continue;

    if (!isPidSupported(pid)) continue;

    (void)def;
    sendObdPid(pid);
    break;
  }
}

void ObdPoller::onRxFrame(const CanFrame& f) {
  uint8_t payload[16];
  uint8_t payloadLen = 0;

  if (!parseIsoTpSingleFrame(f, payload, payloadLen)) {
    return;
  }

  // OBD
  if (matchObdResponse(f)) {
    handleObdResponse(f, payload, payloadLen);
    return;
  }

  // UDS
  if (matchUdsResponse(f)) {
    handleUdsResponse(f, payload, payloadLen);
    return;
  }
}

void ObdPoller::handleObdResponse(const CanFrame& f, const uint8_t* payload, uint8_t payloadLen) {
  if (payloadLen < 2) return;

  const uint8_t service = payload[0];
  if (service != 0x41) {
    // Não é resposta do Service 01
    return;
  }

  const uint8_t pid = payload[1];

  // PIDs de suporte (00/20/40/60)
  if ((pid == 0x00 || pid == 0x20 || pid == 0x40 || pid == 0x60) && payloadLen >= 6) {
    markSupportedPids(pid, &payload[2]);
    _gotSupportBitmap = true;

    if (AppConfig::SERIAL_VERBOSE_PID_BITMAP) {
      Serial.printf("[OBD] supported bitmap pid=0x%02X from 0x%03lX => %02X %02X %02X %02X\n",
                    pid, (unsigned long)f.id, payload[2], payload[3], payload[4], payload[5]);
    }
    return;
  }

  const PidDef* def = findPid(pid);
  if (!def) return;

  float value = 0.0f;
  bool ok = false;

  switch (pid) {
    case 0x0C: // RPM = (256A + B)/4
      if (payloadLen >= 4) {
        value = ((256.0f * payload[2]) + payload[3]) / 4.0f;
        ok = true;
      }
      break;

    case 0x0D: // speed = A (km/h)
      if (payloadLen >= 3) {
        value = payload[2];
        ok = true;
      }
      break;

    case 0x11: // throttle % = A/2.55
      if (payloadLen >= 3) {
        value = payload[2] / 2.55f;
        ok = true;
      }
      break;

    case 0x42: // module voltage = (256A + B)/1000
      if (payloadLen >= 4) {
        value = ((256.0f * payload[2]) + payload[3]) / 1000.0f;
        ok = true;
      }
      break;

    case 0x5A: // accel pedal % = A/2.55
    case 0x5B: // hybrid/EV battery pack charge % = A/2.55
      if (payloadLen >= 3) {
        value = payload[2] / 2.55f;
        ok = true;
      }
      break;

    case 0x51: // fuel type enum
      if (payloadLen >= 3) {
        value = (float)payload[2];
        ok = true;
      }
      break;

    case 0x1F: // runtime seconds = 256A + B
      if (payloadLen >= 4) {
        value = (256.0f * payload[2]) + payload[3];
        ok = true;
      }
      break;

    default:
      break;
  }

  if (!ok) return;

  _sd.logObdValue(f.ts_us, f.id, service, pid, def->name, value, def->unit);

  if (AppConfig::SERIAL_VERBOSE_OBD) {
    if (pid == 0x51) {
      Serial.printf("[OBD] src=0x%03lX pid=0x%02X %-36s = %u (%s)\n",
                    (unsigned long)f.id, pid, def->name, (unsigned)value, def->unit);
    } else {
      Serial.printf("[OBD] src=0x%03lX pid=0x%02X %-36s = %.3f %s\n",
                    (unsigned long)f.id, pid, def->name, value, def->unit);
    }
  }
}

void ObdPoller::handleUdsResponse(const CanFrame& f, const uint8_t* payload, uint8_t payloadLen) {
  if (payloadLen < 1) return;

  const uint8_t service = payload[0];

  // UDS Positive response: service + 0x40
  if (service == 0x62 && payloadLen >= 3) {
    const uint16_t did = ((uint16_t)payload[1] << 8) | payload[2];
    const uint8_t* data = &payload[3];
    const uint8_t dataLen = payloadLen - 3;

    _sd.logUdsRaw(f.ts_us, f.id, did, data, dataLen);

    if (AppConfig::SERIAL_VERBOSE_UDS) {
      Serial.printf("[UDS] src=0x%03lX DID=0x%04X len=%u data=",
                    (unsigned long)f.id, did, (unsigned)dataLen);
      for (uint8_t i = 0; i < dataLen; i++) {
        Serial.printf("%02X", data[i]);
        if (i + 1 < dataLen) Serial.print(' ');
      }
      Serial.println();
    }
    return;
  }

  // Negative response: 0x7F <origService> <NRC>
  if (service == 0x7F && payloadLen >= 3) {
    const uint8_t orig = payload[1];
    const uint8_t nrc  = payload[2];
    if (AppConfig::SERIAL_VERBOSE_UDS) {
      Serial.printf("[UDS] NEGATIVE src=0x%03lX orig=0x%02X nrc=0x%02X\n",
                    (unsigned long)f.id, orig, nrc);
    }
    return;
  }
}
