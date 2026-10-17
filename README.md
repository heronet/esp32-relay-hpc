# ESP32 PC power button with Blynk

Firmware for an ESP32 DOIT DevKit V1 that connects to your Wi-Fi and lets you
briefly close a relay across a desktop motherboard's power-button header from
the Blynk mobile app. It automatically retries Wi-Fi and Blynk connections and
logs boot, connection changes, and relay actions. Routine status is printed to
USB Serial every 10 seconds; cloud telemetry updates every five minutes.

## Wiring

Use a relay module with a **3.3 V compatible control input** and isolated dry
contacts. The default is an active-low module; change `RELAY_ACTIVE_LOW` in
`include/config.h` if yours is active-high.

| Connection              | Destination                          |
| ----------------------- | ------------------------------------ |
| ESP32 / ESP32-S3 GPIO16 | Relay module IN                      |
| ESP32 GND               | Relay module control GND             |
| Relay module VCC        | Supply required by your relay module |
| Relay COM and NO        | The motherboard's two PWR_SW pins    |

Connect COM/NO in parallel with the existing case power button so both work.
Use the motherboard manual to identify PWR_SW. These are contact closures:
**do not inject ESP32 or relay supply voltage into the motherboard pins** and
do not connect these contacts to mains power. Do not drive a bare relay coil
directly from a GPIO.

Keep the ESP32 powered while the PC is off, for example from a separate USB
adapter. Ensure the relay input has an appropriate hardware bias to its inactive
level during ESP32 reset and power-up (a compatible pull-up for active-low,
pull-down for active-high). GPIOs are not driven throughout reset; firmware
alone cannot prevent a boot-time relay glitch. Do not pull an ESP32 GPIO to 5 V.
Test relay behavior with the motherboard disconnected before connecting it.

The default press lasts 500 ms. **This operates a power button, not an ON-only
command:** pressing it while the PC is running may suspend or shut down the PC,
depending on its OS settings. There is no power-state sensor, so the reported
status confirms the button action, not whether the computer booted.

## Configure Blynk and Wi-Fi

1. Create a Blynk template for ESP32 with Wi-Fi connectivity, then create a
   device from that template.
2. Copy `include/secrets.example.h` to `include/secrets.h`. Fill in your template
   ID, template name, device auth token, Wi-Fi SSID, and Wi-Fi password.
   `secrets.h` is ignored by Git; never share it or the firmware containing it.
   Use a 2.4 GHz Wi-Fi network supported by the ESP32.
3. Create these virtual-pin datastreams and dashboard widgets:

| Pin | Type / range            | Widget and purpose                                   |
| --- | ----------------------- | ---------------------------------------------------- |
| V0  | Integer, 0–1, default 0 | Button in **Push** mode: `Press PC power button`     |
| V1  | String                  | Label/value: action status; PC state remains unknown |
| V2  | Integer, -127–0         | Value/gauge: Wi-Fi RSSI in dBm                       |
| V3  | Integer, 0–2147483647   | Value: ESP32 uptime in seconds                       |
| V4  | String                  | Terminal: status log (output only)                   |
| V5  | Integer, 0–2147483647   | Value: accepted presses since ESP32 boot             |

4. Leave state synchronization on connection **disabled for V0**. Do not attach
   automations that restore or replay its value. Firmware resets V0 to zero on
   each Blynk connection and does not request saved button state. Wait three
   seconds after a connection before pressing it.
5. Use the dashboard in the Blynk app to send a press. Additional presses are
   ignored for 10 seconds. Internet access is required for Blynk remote control.
   The ESP32 uses outbound connections; router port forwarding is unnecessary.

The template, device, and dashboard need to be created in your Blynk account;
this repository supplies the firmware and configuration instructions.

## Build, upload, and view logs

With PlatformIO installed, run from this directory:

```sh
cp include/secrets.example.h include/secrets.h
# Edit include/secrets.h before uploading.
pio run
pio run --target upload
pio device monitor --baud 115200
```

If `pio` is not on your PATH, use the PlatformIO extension's terminal or
`~/.platformio/penv/bin/pio`. If several boards are attached, specify the intended
serial port with `--upload-port PORT` for upload and `--port PORT` for monitoring.

The project builds without `secrets.h`, but that build keeps the relay inactive
and prints a setup message instead of connecting. The platform and Blynk
versions are pinned in `platformio.ini`.

## Hardware checks

Before use, verify these hardware behaviors with the motherboard disconnected:

1. Boot/reset leaves the relay released; missing credentials do not activate it.
2. One Blynk button press closes the contacts for approximately 500 ms.
3. Repeated presses within 10 seconds do not create another pulse.
4. Disabling Wi-Fi during a pulse still releases the contacts. Restoring Wi-Fi
   reconnects both services and does not issue a power press.
5. An internet outage produces Serial logs, and recent buffered logs arrive in
   Blynk after reconnection.
6. With an idle connection, Serial status continues every 10 seconds without
   new Terminal lines; V2/V3 refresh every five minutes. A power press updates
   V5 immediately, and reconnecting refreshes all four dashboard values once.

Hardware timing, network recovery, and actual PC startup require a configured
ESP32 and relay; compilation alone does not validate those behaviors.

References: [Blynk virtual pins](https://docs.blynk.io/en/getting-started/using-virtual-pins-to-control-physical-devices),
[Blynk connection management](https://docs.blynk.io/en/blynk-library-firmware-api/connection-management),
[ESP32 Wi-Fi API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html).
