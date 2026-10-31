#pragma once

#include <stdint.h>

// Original hardware manual: channel 1 D13/D2, channel 2 D12/D4.
// HIGH drives the control optocoupler; LOW on the sense input means LED on.
struct PcConfig {
  const char *name;
  uint8_t switchPin;
  uint8_t sensePin;
  uint8_t buttonVPin;
  uint8_t statusVPin;
  uint8_t countVPin;
};

constexpr PcConfig PC_CONFIGS[] = {
    {"PC 1", 13, 2, 0, 1, 5},
    {"PC 2", 12, 4, 6, 7, 8},
};
constexpr uint8_t PC_COUNT = sizeof(PC_CONFIGS) / sizeof(PC_CONFIGS[0]);
constexpr bool POWER_SWITCH_ACTIVE_LOW = false;
constexpr bool POWER_SENSE_ACTIVE_LOW = true;
constexpr uint32_t POWER_SENSE_DEBOUNCE_MS = 2000;
constexpr uint32_t POWER_PULSE_MS = 500;
constexpr uint32_t PRESS_COOLDOWN_MS = 10000;
constexpr uint32_t CONNECT_GUARD_MS = 3000;
constexpr uint32_t WIFI_RETRY_MS = 15000;
constexpr uint32_t BLYNK_RETRY_MS = 10000;
// Allow time for TLS, Blynk regional redirects, and the login response.
constexpr uint32_t BLYNK_CONNECT_TIMEOUT_MS = 30000;
constexpr uint32_t SERIAL_STATUS_INTERVAL_MS = 10000;
constexpr uint32_t CLOUD_TELEMETRY_INTERVAL_MS = 5 * 60 * 1000;
constexpr char DEVICE_HOSTNAME[] = "esp32-pc-power";

static_assert(PC_COUNT == 2, "Blynk command handlers are defined for two PCs");
static_assert(PC_CONFIGS[0].buttonVPin == 0 && PC_CONFIGS[1].buttonVPin == 6,
              "Button virtual pins must match the V0 and V6 command handlers");
static_assert(PC_CONFIGS[0].switchPin != PC_CONFIGS[0].sensePin &&
                  PC_CONFIGS[1].switchPin != PC_CONFIGS[1].sensePin &&
                  PC_CONFIGS[0].switchPin != PC_CONFIGS[1].switchPin &&
                  PC_CONFIGS[0].sensePin != PC_CONFIGS[1].sensePin &&
                  PC_CONFIGS[0].switchPin != PC_CONFIGS[1].sensePin &&
                  PC_CONFIGS[1].switchPin != PC_CONFIGS[0].sensePin,
              "Both PCs require distinct control and sensing GPIOs");
static_assert(POWER_PULSE_MS >= 100 && POWER_PULSE_MS <= 1000,
              "Use a short power-button pulse, not a forced shutdown hold");
