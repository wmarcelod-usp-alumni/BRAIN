#pragma once
#include <Arduino.h>

template <typename T, size_t N>
class RingBuffer {
public:
  void push(const T &v) {
    _buf[_head] = v;
    _head = (_head + 1) % N;
    if (_count < N) _count++;
  }

  size_t size() const { return _count; }

  // 0 = mais novo, 1 = anterior, ...
  bool getNewest(size_t indexFromNewest, T &out) const {
    if (indexFromNewest >= _count) return false;
    size_t idx = (_head + N - 1 - indexFromNewest) % N;
    out = _buf[idx];
    return true;
  }

private:
  T _buf[N];
  size_t _head = 0;
  size_t _count = 0;
};
