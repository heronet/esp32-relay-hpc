#define BLYNK_PRINT Serial
#include <Arduino.h>
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif
// Only the guarded virtual button may control the optocoupler, never direct GPIO
// commands.
#define BLYNK_NO_BUILTIN
#include "config.h"
#include "press_guard.h"
#include <BlynkSimpleEsp32_SSL.h>
#include <atomic>
#include <esp_timer.h>
#include <stdarg.h>
#include <time.h>

namespace {
constexpr uint8_t LOG_CAPACITY = 24;
char logs[LOG_CAPACITY][160];
uint8_t logHead = 0, logCount = 0;
uint32_t droppedLogs = 0;
uint32_t lastWifiTry = 0, lastBlynkTry = 0, connectedAt = 0;
uint32_t lastStatus = 0, lastLogSend = 0, lastTelemetry = 0;
bool configured = false, wifiWasConnected = false, blynkWasConnected = false;
bool timeWasReady = false;
struct PcState {
  const PcConfig *config = nullptr;
  std::atomic<bool> pulseActive{false};
  std::atomic<bool> pulseFinished{false};
  esp_timer_handle_t releaseTimer = nullptr;
  PressGuard pressGuard;
  uint32_t pressCount = 0, publishedPressCount = 0;
  char publishedStatus[128] = "";
  const char *actionStatus = "Ready";
  bool powerSenseCandidate = false, powerLedOn = false, powerSenseKnown = false;
  uint32_t powerSenseChangedAt = 0, lastPowerSenseSample = 0;
};

// Static lifetime: each timer callback receives its own stable state pointer.
PcState pcs[PC_COUNT];

void writeLog(bool toCloud, const char *format, va_list args) {
  char message[128];
  vsnprintf(message, sizeof(message), format, args);
  char line[sizeof(logs[0])];
  snprintf(line, sizeof(line), "[%llu s] %s",
           static_cast<unsigned long long>(esp_timer_get_time() / 1000000),
           message);
  Serial.println(line);
  if (!toCloud)
    return;
  if (logCount == LOG_CAPACITY) {
    logHead = (logHead + 1) % LOG_CAPACITY;
    --logCount;
    ++droppedLogs;
  }
  strcpy(logs[(logHead + logCount) % LOG_CAPACITY], line);
  ++logCount;
}

// Events are retained for the Blynk Terminal. Routine diagnostics stay local.
void logStatus(const char *format, ...) {
  va_list args;
  va_start(args, format);
  writeLog(true, format, args);
  va_end(args);
}

void logSerial(const char *format, ...) {
  va_list args;
  va_start(args, format);
  writeLog(false, format, args);
  va_end(args);
}

// Runs on the ESP timer task. Never call network or logging functions here.
void releasePowerButton(void *arg) {
  PcState &pc = *static_cast<PcState *>(arg);
  digitalWrite(pc.config->switchPin, POWER_SWITCH_ACTIVE_LOW ? HIGH : LOW);
  pc.pulseActive.store(false);
  pc.pulseFinished.store(true);
}

const char *powerStatus(const PcState &pc) {
  if (!pc.powerSenseKnown)
    return "PC state unknown";
  return pc.powerLedOn ? "PC ON (power LED on)" : "PC OFF/asleep (power LED off)";
}

void publishState(PcState &pc, bool force = false) {
  if (!Blynk.connected())
    return;
  char status[sizeof(pc.publishedStatus)];
  snprintf(status, sizeof(status), "%s | %s", powerStatus(pc), pc.actionStatus);
  if (force || strcmp(pc.publishedStatus, status) != 0) {
    Blynk.virtualWrite(pc.config->statusVPin, status);
    strcpy(pc.publishedStatus, status);
  }
  if (force || pc.publishedPressCount != pc.pressCount) {
    Blynk.virtualWrite(pc.config->countVPin, pc.pressCount);
    pc.publishedPressCount = pc.pressCount;
  }
}

void servicePowerSense(PcState &pc) {
  const uint32_t now = millis();
  // Network calls can block. Do not count an unobserved interval as stable.
  if (uint32_t(now - pc.lastPowerSenseSample) > 250)
    pc.powerSenseChangedAt = now;
  pc.lastPowerSenseSample = now;
  const bool sample = digitalRead(pc.config->sensePin) ==
                      (POWER_SENSE_ACTIVE_LOW ? LOW : HIGH);
  if (sample != pc.powerSenseCandidate) {
    pc.powerSenseCandidate = sample;
    pc.powerSenseChangedAt = now;
  }
  // Require a stable indication to avoid reacting to short LED flickers.
  if ((!pc.powerSenseKnown || pc.powerLedOn != pc.powerSenseCandidate) &&
      uint32_t(now - pc.powerSenseChangedAt) >= POWER_SENSE_DEBOUNCE_MS) {
    pc.powerLedOn = pc.powerSenseCandidate;
    pc.powerSenseKnown = true;
    logStatus("%s power indication: %s", pc.config->name, powerStatus(pc));
    publishState(pc);
  }
}

void publishTelemetry() {
  if (!Blynk.connected())
    return;
  Blynk.virtualWrite(V2, WiFi.RSSI());
  Blynk.virtualWrite(V3, static_cast<uint32_t>(esp_timer_get_time() / 1000000));
  lastTelemetry = millis();
}

void requestPowerPress(PcState &pc) {
  const uint32_t now = millis();
  if (pc.releaseTimer == nullptr || pc.pulseActive.load() ||
      !pc.pressGuard.ready(now, connectedAt, CONNECT_GUARD_MS,
                          PRESS_COOLDOWN_MS)) {
    logStatus("%s press ignored: control unavailable, busy, or cooldown",
              pc.config->name);
    return;
  }
  pc.pulseActive.store(true);
  digitalWrite(pc.config->switchPin, POWER_SWITCH_ACTIVE_LOW ? LOW : HIGH);
  if (esp_timer_start_once(pc.releaseTimer, POWER_PULSE_MS * 1000ULL) != ESP_OK) {
    digitalWrite(pc.config->switchPin, POWER_SWITCH_ACTIVE_LOW ? HIGH : LOW);
    pc.pulseActive.store(false);
    pc.actionStatus = "Control timer error; press not confirmed";
    logStatus("ERROR: %s release timer failed; button released immediately",
              pc.config->name);
    return;
  }
  pc.pressGuard.record(now);
  ++pc.pressCount;
  pc.actionStatus = "Power-button press sent";
  logStatus("%s power-button pulse started (%lu ms)",
            pc.config->name, (unsigned long)POWER_PULSE_MS);
}

void handlePowerCommand(PcState &pc) {
  requestPowerPress(pc);
  Blynk.virtualWrite(pc.config->buttonVPin, 0);
  publishState(pc);
}

void serviceNetwork() {
  const uint32_t now = millis();
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  if (!wifiConnected) {
    if (wifiWasConnected) {
      logStatus("Wi-Fi disconnected; reconnecting");
      Blynk.disconnect();
    }
    wifiWasConnected = false;
    if (uint32_t(now - lastWifiTry) >= WIFI_RETRY_MS) {
      lastWifiTry = now;
      logSerial("Retrying Wi-Fi connection");
      WiFi.reconnect();
    }
    return;
  }
  if (!wifiWasConnected) {
    wifiWasConnected = true;
    logStatus("Wi-Fi connected: IP %s, signal %d dBm",
              WiFi.localIP().toString().c_str(), WiFi.RSSI());
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    lastBlynkTry = now - BLYNK_RETRY_MS;
    logSerial("Synchronizing clock for certificate verification");
  }
  // TLS verification needs a valid clock. SNTP retries in the background.
  if (time(nullptr) < 1700000000)
    return;
  if (!timeWasReady) {
    timeWasReady = true;
    logSerial("Clock ready");
  }
  if (!Blynk.connected() && uint32_t(now - lastBlynkTry) >= BLYNK_RETRY_MS) {
    logSerial("Connecting to Blynk over TLS");
    // DNS/TLS may take longer than this protocol timeout. The button's
    // independent release timer continues to run during network calls.
    if (!Blynk.connect(BLYNK_CONNECT_TIMEOUT_MS)) {
      if (Blynk.isTokenInvalid()) {
        logStatus(
            "Blynk rejected the device auth token; check include/secrets.h");
      } else {
        logStatus(
            "Blynk connection failed; see Serial connection diagnostics above");
      }
      // End this attempt cleanly before the scheduled retry.
      Blynk.disconnect();
    }
    lastBlynkTry = millis();
  }
  if (Blynk.connected())
    Blynk.run();
}
} // namespace

BLYNK_CONNECTED() {
  connectedAt = millis();
  logStatus("Blynk connected; controls ready in %lu seconds",
            (unsigned long)(CONNECT_GUARD_MS / 1000));
  // Never sync V0/V6: restoring old values could press either power button.
  for (PcState &pc : pcs)
    Blynk.virtualWrite(pc.config->buttonVPin, 0);
  // Refresh the dashboard once per connection, then only publish changes.
  for (PcState &pc : pcs)
    publishState(pc, true);
  publishTelemetry();
  // Drain event logs after the initial dashboard updates, not in the same burst.
  lastLogSend = millis();
}

BLYNK_WRITE(V0) {
  if (param.asInt() != 1)
    return;
  handlePowerCommand(pcs[0]);
}

BLYNK_WRITE(V6) {
  if (param.asInt() != 1)
    return;
  handlePowerCommand(pcs[1]);
}

void setup() {
  // Establish both inactive outputs before starting Wi-Fi or Serial.
  for (uint8_t i = 0; i < PC_COUNT; ++i) {
    PcState &pc = pcs[i];
    pc.config = &PC_CONFIGS[i];
    digitalWrite(pc.config->switchPin, POWER_SWITCH_ACTIVE_LOW ? HIGH : LOW);
    pinMode(pc.config->switchPin, OUTPUT);
    // Supplement the external 3.3 V pull-up shown in the manual.
    pinMode(pc.config->sensePin, INPUT_PULLUP);
    pc.powerSenseCandidate = digitalRead(pc.config->sensePin) ==
                             (POWER_SENSE_ACTIVE_LOW ? LOW : HIGH);
    pc.powerSenseChangedAt = millis();
  }
  Serial.begin(115200);
  for (PcState &pc : pcs) {
    logStatus("Boot: %s button released (GPIO%u); power sensing on GPIO%u",
              pc.config->name, pc.config->switchPin, pc.config->sensePin);
    esp_timer_create_args_t timerArgs = {};
    timerArgs.callback = releasePowerButton;
    timerArgs.arg = &pc;
    timerArgs.name = pc.config->name;
    if (esp_timer_create(&timerArgs, &pc.releaseTimer) != ESP_OK) {
      pc.releaseTimer = nullptr;
      pc.actionStatus = "Control disabled: timer creation failed";
      logStatus("ERROR: could not create %s release timer", pc.config->name);
    }
  }
  configured = strlen(WIFI_SSID) > 0 && strlen(BLYNK_AUTH_TOKEN) > 0 &&
               strncmp(WIFI_SSID, "YOUR_", 5) != 0 &&
               strncmp(BLYNK_AUTH_TOKEN, "YOUR_", 5) != 0 &&
               strncmp(BLYNK_TEMPLATE_ID, "YOUR_", 5) != 0;
  if (!configured) {
    logStatus("Setup required: copy include/secrets.example.h to secrets.h and "
              "edit it");
    return;
  }
  WiFi.persistent(false);
  WiFi.setHostname(DEVICE_HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  Blynk.config(BLYNK_AUTH_TOKEN);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiTry = millis();
  logSerial("Connecting to configured Wi-Fi");
}

void loop() {
  for (PcState &pc : pcs) {
    if (pc.pulseFinished.exchange(false))
      logStatus("%s power button released; pulse complete", pc.config->name);
    servicePowerSense(pc);
  }
  if (!configured) {
    delay(10);
    return;
  }
  serviceNetwork();
  for (PcState &pc : pcs)
    servicePowerSense(pc);
  const bool connected = Blynk.connected();
  if (blynkWasConnected && !connected)
    logStatus("Blynk disconnected; will retry");
  blynkWasConnected = connected;
  const uint32_t now = millis();
  if (uint32_t(now - lastStatus) >= SERIAL_STATUS_INTERVAL_MS) {
    lastStatus = now;
    logSerial("Status: Wi-Fi %s, Blynk %s",
              WiFi.status() == WL_CONNECTED ? "connected" : "offline",
              connected ? "connected" : "offline");
    for (const PcState &pc : pcs) {
      logSerial("%s: button %s, presses %lu, %s", pc.config->name,
                pc.pulseActive.load() ? "active" : "released",
                (unsigned long)pc.pressCount, powerStatus(pc));
    }
  }
  if (connected &&
      uint32_t(now - lastTelemetry) >= CLOUD_TELEMETRY_INTERVAL_MS) {
    publishTelemetry();
  }
  // Bound traffic and retain recent offline logs in RAM for reconnection.
  if (connected && uint32_t(now - lastLogSend) >= 1000) {
    lastLogSend = now;
    if (droppedLogs) {
      Blynk.virtualWrite(V4, String("[log] Older lines overwritten: ") +
                                 droppedLogs + "\n");
      droppedLogs = 0;
    } else if (logCount) {
      Blynk.virtualWrite(V4, String(logs[logHead]) + "\n");
      logHead = (logHead + 1) % LOG_CAPACITY;
      --logCount;
    }
  }
  delay(2);
}
