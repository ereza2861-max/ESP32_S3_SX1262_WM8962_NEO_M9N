#include <cassert>
#include <cstdint>
#include "Config.h"

struct Range {
  const char* name;
  uint32_t minValue;
  uint32_t maxValue;
};

int main() {
  const Range ranges[] = {
      {"wakePeriodSec", Config::WAKE_PERIOD_SEC_MIN, Config::WAKE_PERIOD_SEC_MAX},
      {"deepSleepIdleMs", Config::DEEP_SLEEP_IDLE_MS_MIN, Config::DEEP_SLEEP_IDLE_MS_MAX},
      {"mqttReconnectMs", Config::MQTT_RECONNECT_MS_MIN, Config::MQTT_RECONNECT_MS_MAX},
      {"mqttTelemetryPeriodMs", Config::MQTT_TELEMETRY_PERIOD_MS_MIN,
       Config::MQTT_TELEMETRY_PERIOD_MS_MAX},
      {"bleScanIntervalMs", Config::BLE_SCAN_INTERVAL_MS_MIN, Config::BLE_SCAN_INTERVAL_MS_MAX},
      {"webSessionTimeoutMs", Config::WEB_SESSION_TIMEOUT_MS_MIN,
       Config::WEB_SESSION_TIMEOUT_MS_MAX},
      {"webAuthRateLimitMs", Config::WEB_AUTH_RATE_LIMIT_MS_MIN,
       Config::WEB_AUTH_RATE_LIMIT_MS_MAX},
      {"replayWindowBits", Config::LORA_REPLAY_WINDOW_BITS_MIN, Config::LORA_REPLAY_WINDOW_BITS},
      {"certRenewalThresholdDays", Config::CERT_RENEWAL_THRESHOLD_DAYS_MIN,
       Config::CERT_RENEWAL_THRESHOLD_DAYS_MAX},
  };

  for (const auto& range : ranges) {
    assert(range.minValue <= range.maxValue);
  }

  assert(Config::BATTERY_CRITICAL_THRESHOLD_MIN == 2.5f);
  assert(Config::BATTERY_LOW_THRESHOLD_MAX == 4.2f);
  assert(Config::BATTERY_CRITICAL_THRESHOLD_MIN < Config::BATTERY_LOW_THRESHOLD_MAX);
  assert(Config::DEEP_SLEEP_IDLE_MS_MIN % 1000UL == 0);
  assert(Config::DEEP_SLEEP_IDLE_MS_MAX % 1000UL == 0);

  // These are the exact values currently accepted by both the WebUI parser
  // and RuntimeConfig validation. Keeping this test at the shared contract
  // prevents silent UI/backend boundary drift.
  assert(Config::WAKE_PERIOD_SEC_MIN == 60UL);
  assert(Config::WAKE_PERIOD_SEC_MAX == 604800UL);
  assert(Config::DEEP_SLEEP_IDLE_MS_MIN == 60000UL);
  assert(Config::DEEP_SLEEP_IDLE_MS_MAX == 86400000UL);
  assert(Config::MQTT_RECONNECT_MS_MIN == 1000UL);
  assert(Config::MQTT_RECONNECT_MS_MAX == 3600000UL);
  assert(Config::MQTT_TELEMETRY_PERIOD_MS_MIN == 1000UL);
  assert(Config::MQTT_TELEMETRY_PERIOD_MS_MAX == 86400000UL);
  assert(Config::BLE_SCAN_INTERVAL_MS_MIN == 100UL);
  assert(Config::BLE_SCAN_INTERVAL_MS_MAX == 60000UL);
  assert(Config::WEB_SESSION_TIMEOUT_MS_MIN == 60000UL);
  assert(Config::WEB_SESSION_TIMEOUT_MS_MAX == 86400000UL);
  assert(Config::WEB_AUTH_RATE_LIMIT_MS_MIN == 100UL);
  assert(Config::WEB_AUTH_RATE_LIMIT_MS_MAX == 600000UL);
  assert(Config::LORA_REPLAY_WINDOW_BITS_MIN == 8);
  assert(Config::CERT_RENEWAL_THRESHOLD_DAYS_MIN == 1);
  assert(Config::CERT_RENEWAL_THRESHOLD_DAYS_MAX == 3650);
  assert(Config::STA_SSID_MAX_LEN == 32);
  assert(Config::STA_PASSWORD_MIN_LEN == 8);
  assert(Config::STA_PASSWORD_MAX_LEN == 63);
  assert(Config::STA_PASSWORD_MIN_LEN <= Config::STA_PASSWORD_MAX_LEN);

  return 0;
}
