#pragma once
#include <Arduino.h>

class FragmentArq {
public:
  bool begin();
  void task();

private:
  // TODO: selective-repeat bitmap/timeout state for LORA_TYPE_TEXT_ACK.
};
