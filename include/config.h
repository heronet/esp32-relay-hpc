#pragma once

#include <stdint.h>

// GPIO16 for the target ESP32 and temporary ESP32-S3.
// Use a 3.3 V compatible relay module.
constexpr uint8_t RELAY_PIN = 16;
constexpr bool RELAY_ACTIVE_LOW = true;
constexpr uint32_t RELAY_PULSE_MS = 500;
constexpr uint32_t PRESS_COOLDOWN_MS = 10000;
constexpr uint32_t CONNECT_GUARD_MS = 3000;
constexpr uint32_t WIFI_RETRY_MS = 15000;
constexpr uint32_t BLYNK_RETRY_MS = 10000;
// Allow time for TLS, Blynk regional redirects, and the login response.
constexpr uint32_t BLYNK_CONNECT_TIMEOUT_MS = 30000;
constexpr uint32_t SERIAL_STATUS_INTERVAL_MS = 10000;
constexpr uint32_t CLOUD_TELEMETRY_INTERVAL_MS = 5 * 60 * 1000;
constexpr char DEVICE_HOSTNAME[] = "esp32-pc-power";

static_assert(RELAY_PULSE_MS >= 100 && RELAY_PULSE_MS <= 1000,
              "Use a short power-button pulse, not a forced shutdown hold");
