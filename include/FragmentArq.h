#pragma once
#include <Arduino.h>

class FragmentArq {
public:
  bool begin();
  void task();

private:
  bool enabled_ = false;
};
