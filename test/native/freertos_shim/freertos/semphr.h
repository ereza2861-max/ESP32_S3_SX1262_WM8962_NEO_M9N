#pragma once
#include "FreeRTOS.h"
using SemaphoreHandle_t = std::mutex*;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::mutex(); }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t) {
  if (!mutex) return pdFALSE;
  mutex->lock();
  return pdTRUE;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) {
  if (!mutex) return pdFALSE;
  mutex->unlock();
  return pdTRUE;
}
