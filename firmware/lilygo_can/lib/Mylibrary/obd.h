#pragma once
#include <Arduino.h>
#include "types.h"

class CanTwai;

struct ObdValue {
  bool valid = false;
  uint32_t last_ms = 0;
  float value = 0.0f;
};

class ObdManager {
public:
  void setEnabled(bool en) { _enabled = en; }
  void setExtended29(bool en) { _ext29 = en; }
  bool enabled() const { return _enabled; }

  bool requestPid(uint8_t pid, CanTwai& can);
  void onFrame(const CanFrame& f);

  void toJson(char* out, size_t outLen) const;

private:
  bool _enabled = false;
  bool _ext29 = false;

  ObdValue _voltage, _speed, _rpm, _throttle, _coolant;

  void store(ObdValue& v, float val);
  uint32_t ageMs(const ObdValue& v) const;
  bool isStdResp(const CanFrame& f) const;
  bool is29Resp(const CanFrame& f) const;
  void parseSingleFrame(const CanFrame& f);
};
