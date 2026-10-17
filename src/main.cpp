#define BLYNK_PRINT Serial
#include <Arduino.h>
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif
// Only the guarded virtual button may control the relay, never direct GPIO
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
uint32_t lastStatus = 0, lastLogSend = 0, pressCount = 0;
uint32_t lastTelemetry = 0, publishedPressCount = 0;
const char *publishedActionStatus = nullptr;
bool configured = false, wifiWasConnected = false, blynkWasConnected = false;
bool timeWasReady = false;
std::atomic<bool> pulseActive{false};
std::atomic<bool> pulseFinished{false};
esp_timer_handle_t releaseTimer = nullptr;
PressGuard pressGuard;
const char *actionStatus = "Ready; PC state unknown";

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
void releaseRelay(void *) {
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  pulseActive.store(false);
  pulseFinished.store(true);
}

void publishState(bool force = false) {
  if (!Blynk.connected())
    return;
  if (force || publishedActionStatus == nullptr ||
      strcmp(publishedActionStatus, actionStatus) != 0) {
    Blynk.virtualWrite(V1, actionStatus);
    publishedActionStatus = actionStatus;
  }
  if (force || publishedPressCount != pressCount) {
    Blynk.virtualWrite(V5, pressCount);
    publishedPressCount = pressCount;
  }
}

void publishTelemetry() {
  if (!Blynk.connected())
    return;
  Blynk.virtualWrite(V2, WiFi.RSSI());
  Blynk.virtualWrite(V3, static_cast<uint32_t>(esp_timer_get_time() / 1000000));
  lastTelemetry = millis();
}

void requestPowerPress() {
  const uint32_t now = millis();
  if (releaseTimer == nullptr || pulseActive.load() ||
      !pressGuard.ready(now, connectedAt, CONNECT_GUARD_MS,
                        PRESS_COOLDOWN_MS)) {
    logStatus("Power press ignored: relay unavailable, busy, or cooldown");
    return;
  }
  pulseActive.store(true);
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? LOW : HIGH);
  if (esp_timer_start_once(releaseTimer, RELAY_PULSE_MS * 1000ULL) != ESP_OK) {
    digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
    pulseActive.store(false);
    actionStatus = "Relay timer error; press not confirmed";
    logStatus("ERROR: release timer failed; relay released immediately");
    return;
  }
  pressGuard.record(now);
  ++pressCount;
  actionStatus = "Press sent; PC state unknown";
  logStatus("Power-button pulse started (%lu ms)",
            (unsigned long)RELAY_PULSE_MS);
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
    // DNS/TLS may take longer than this protocol timeout. The relay's
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
  // Never sync V0: restoring an old button value could press the power button.
  Blynk.virtualWrite(V0, 0);
  // Refresh the dashboard once per connection, then only publish changes.
  publishState(true);
  publishTelemetry();
}

BLYNK_WRITE(V0) {
  if (param.asInt() != 1)
    return;
  requestPowerPress();
  Blynk.virtualWrite(V0, 0);
  publishState();
}

void setup() {
  // Establish the inactive output before starting Wi-Fi or Serial.
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  pinMode(RELAY_PIN, OUTPUT);
  Serial.begin(115200);
  logStatus("Boot: relay released; PC power state is not measured");
  esp_timer_create_args_t timerArgs = {};
  timerArgs.callback = releaseRelay;
  timerArgs.name = "relay-release";
  if (esp_timer_create(&timerArgs, &releaseTimer) != ESP_OK) {
    releaseTimer = nullptr;
    actionStatus = "Relay disabled: timer creation failed";
    logStatus("ERROR: could not create relay release timer");
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
  if (pulseFinished.exchange(false))
    logStatus("Relay released; power-button pulse complete");
  if (!configured) {
    delay(10);
    return;
  }
  serviceNetwork();
  const bool connected = Blynk.connected();
  if (blynkWasConnected && !connected)
    logStatus("Blynk disconnected; will retry");
  blynkWasConnected = connected;
  const uint32_t now = millis();
  if (uint32_t(now - lastStatus) >= SERIAL_STATUS_INTERVAL_MS) {
    lastStatus = now;
    logSerial("Status: Wi-Fi %s, Blynk %s, relay %s, presses %lu",
              WiFi.status() == WL_CONNECTED ? "connected" : "offline",
              connected ? "connected" : "offline",
              pulseActive.load() ? "active" : "released",
              (unsigned long)pressCount);
  }
  if (connected &&
      uint32_t(now - lastTelemetry) >= CLOUD_TELEMETRY_INTERVAL_MS) {
    publishTelemetry();
  }
  // Bound traffic and retain recent offline logs in RAM for reconnection.
  if (connected && uint32_t(now - lastLogSend) >= 250) {
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
