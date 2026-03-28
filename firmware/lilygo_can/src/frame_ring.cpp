#include "frame_ring.h"

FrameRing::FrameRing(size_t capacity)
: _cap(capacity) {
  _buf = new CanFrame[_cap];
}

void FrameRing::push(const CanFrame& f) {
  _buf[_head] = f;
  _head = (_head + 1) % _cap;
  if (_count < _cap) {
    _count++;
  }
}

size_t FrameRing::copyLatest(CanFrame* out, size_t max) const {
  size_t n = _count;
  if (n > max) n = max;

  // índice do item mais antigo entre os n últimos
  size_t start;
  if (_count < _cap) {
    // buffer ainda não encheu: dados válidos estão em [0, _count)
    start = (_count > n) ? (_count - n) : 0;
  } else {
    // buffer cheio: _head aponta para posição de escrita (mais antigo = _head)
    // queremos os n últimos => começa em (_head + (_cap - n)) % _cap
    start = (_head + (_cap - n)) % _cap;
  }

  for (size_t i = 0; i < n; i++) {
    out[i] = _buf[(start + i) % _cap];
  }
  return n;
}
