#pragma once
#include <Arduino.h>
#include "types.h"

class FrameRing {
public:
  explicit FrameRing(size_t capacity);

  void push(const CanFrame& f);

  // Copia até 'max' frames mais recentes para 'out' (ordem: mais antigo -> mais novo)
  // Retorna quantidade copiada.
  size_t copyLatest(CanFrame* out, size_t max) const;

  size_t capacity() const { return _cap; }

private:
  size_t _cap;
  CanFrame* _buf;
  volatile size_t _head = 0;
  volatile size_t _count = 0;
};
