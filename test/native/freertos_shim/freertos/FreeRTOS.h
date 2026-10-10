#pragma once
#include <cstdint>
#include <mutex>
using TickType_t = uint32_t;
using BaseType_t = int;
using portMUX_TYPE = std::mutex;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>(ms))
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
