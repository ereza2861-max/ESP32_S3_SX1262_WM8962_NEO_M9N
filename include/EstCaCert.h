#pragma once
#include "MqttCaCert.h"

#if __has_include("generated/EstCaCert.h")
#include "generated/EstCaCert.h"
#else
#define EST_CA_CUSTOM 0
#endif

static inline const char* estTrustAnchor() {
#if EST_CA_CUSTOM
  return EST_ROOT_CA;
#else
  return MQTT_BROKER_ROOT_CA;
#endif
}
