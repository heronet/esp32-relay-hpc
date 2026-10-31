# ESP32 PC power button with Blynk

Firmware for an ESP32 DOIT DevKit V1 using channels 1 and 2 of the original HPC board's
PC817 optocoupler circuits. Blynk sends a brief power-button press and displays
the motherboard power-LED indication. It automatically retries Wi-Fi and Blynk
connections and logs boot, connection changes, power indication changes, and
button actions. Routine status is printed to USB Serial every 10 seconds;
cloud telemetry updates every five minutes. Each PC has its own release timer,
10-second press cooldown, power-sense debounce, and press count. Either PC can
be controlled while the other PC's button is pulsing or cooling down.

## Wiring

The configuration follows **Manual.pdf, sections 3.2–3.4, pages 5–7**, as selected
for this hardware. It uses the manual's channel numbering, not inferred PCB
connector numbers. The original Adafruit/MQTT software is not used.

| Connection | Function |
| --- | --- |
| PC 1: D13 / GPIO13; PC 2: D12 / GPIO12 | Drives that PC's control PC817 through its current-limiting resistor; HIGH presses, LOW releases |
| Control PC817 transistor side | Connects across motherboard PWR_SW, in parallel with the case button |
| Motherboard PWR_LED+ / PWR_LED− | Drives the monitoring PC817 input through the board's current-limiting circuit; observe polarity |
| PC 1: D2 / GPIO2; PC 2: D4 / GPIO4 | Reads that PC's monitoring PC817 output; LOW means power LED on, HIGH means LED off |
| ESP32 3.3 V / GND | Monitoring pull-up and ESP32-side circuit reference, as shown in the manual |

Keep the motherboard connections on the isolated side of the optocouplers;
do not connect motherboard LED voltage directly to a GPIO or bridge the
isolation with a common ground. PC817 transistor terminals are polarized,
unlike relay contacts, so retain the original board's switch-output polarity.
The firmware enables internal pull-ups on D2 and D4 in addition to the external
3.3 V pull-up shown in the manual.

Keep the ESP32 powered while both PCs are off. D13 and D12 initialize LOW;
their hardware should also hold the optocouplers inactive during reset, while
the GPIOs are not driven. Verify the outputs without motherboards attached first.

The default press lasts 500 ms. **This operates a power button, not an ON-only
command:** pressing while the PC is running may suspend or shut down the PC,
depending on its OS settings. Power sensing does not automatically suppress
presses or turn the button into an ON/OFF switch.

V1 (PC 1) and V7 (PC 2) combine each PC's last button action with its sensed
power-LED indication. Each
input must remain unchanged across regular samples for two seconds before an
update. Network connection attempts can delay sensing; gaps in sampling restart
the debounce interval. A blinking sleep LED may leave the last stable indication
displayed. An unlit LED or disconnected sensing cable reads OFF/asleep; this is
not proof of operating-system shutdown or successful boot.

The manual's full reference mapping is below. **Channels 1 and 2 are implemented.**
Their GPIO and virtual-pin assignments are in `PC_CONFIGS` in `include/config.h`.
Channels 3–8 are reference only; they have no controls in this firmware.

| Channel | Power-button output | Power-status input |
| --- | --- | --- |
| 1 | D13 | D2 |
| 2 | D12 | D4 |
| 3 | D14 | D5 |
| 4 | D27 | D18 |
| 5 | D26 | D19 |
| 6 | D25 | D21 |
| 7 | D33 | D22 |
| 8 | D32 | D23 |

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
| V0  | Integer, 0–1, default 0 | Button in **Push** mode: `Press PC 1 power button`     |
| V1  | String                  | PC 1 label/value: power-LED indication and last button action |
| V2  | Integer, -127–0         | Value/gauge: Wi-Fi RSSI in dBm                       |
| V3  | Integer, 0–2147483647   | Value: ESP32 uptime in seconds                       |
| V4  | String                  | Shared Terminal: event log, with PC 1 / PC 2 labels (output only) |
| V5  | Integer, 0–2147483647   | PC 1 accepted presses since ESP32 boot             |
| V6  | Integer, 0–1, default 0 | Button in **Push** mode: `Press PC 2 power button` |
| V7  | String                  | PC 2 label/value: power-LED indication and last button action |
| V8  | Integer, 0–2147483647   | PC 2 accepted presses since ESP32 boot |

4. Leave state synchronization on connection **disabled for V0 and V6**. Do not attach
   automations that restore or replay their values. Firmware resets both to zero on
   each Blynk connection and does not request saved button state. Wait three
   seconds after a connection before pressing it.
5. Use the dashboard in the Blynk app to send a press. Additional presses for
   that same PC are ignored for 10 seconds. Internet access is required for Blynk remote control.
   The ESP32 uses outbound connections; router port forwarding is unnecessary.

The template, device, and dashboard need to be created in your Blynk account;
this repository supplies the firmware and configuration instructions.

If PC 1 is already set up, add V6, V7, and V8 to the same template and add their
widgets to your dashboard. Both PCs share the same ESP32 device and auth token.
Existing V0–V5 widgets keep their assignments. Wi-Fi signal and uptime describe
the shared ESP32, not either PC.

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

The project builds without `secrets.h`, but that build keeps both control optocouplers inactive
and prints a setup message instead of connecting. The platform and Blynk
versions are pinned in `platformio.ini`.

V1/V5 (PC 1) and V7/V8 (PC 2) update only on changes or reconnection. V2/V3 update every five
minutes, approximately 17,280 periodic messages per 30 days, plus commands,
events, and reconnections. Ten-second status logs stay on USB Serial. The
Blynk Terminal receives event logs at most once per second, with the latest 24
buffered during outages. PC 2 adds event-driven updates, not another periodic
telemetry stream.

## Hardware checks

First verify the control output with the motherboard disconnected, then verify
sensing using the board's isolated monitoring circuit:

1. Boot/reset leaves D13 and D12 LOW; missing credentials do not activate either optocoupler.
2. V0 drives only D13 HIGH for approximately 500 ms, then LOW. V6 does the same
   for D12. Each pulse must release independently, including overlapping pulses.
3. Repeated presses for one PC within 10 seconds do not create another pulse;
   the other PC's button still works.
4. Disabling Wi-Fi during a pulse still releases the button. Restoring Wi-Fi
   reconnects both services and does not issue a power press.
5. An internet outage produces Serial logs, and recent buffered logs arrive in
   Blynk after reconnection.
6. With an idle connection, Serial status continues every 10 seconds without
   new Terminal lines unless an event occurs; V2/V3 refresh every five minutes. A power press updates
   V5 (PC 1) or V8 (PC 2) immediately. Reconnecting resets both command widgets
   to zero and refreshes both PCs' status/count plus the shared telemetry.
7. PC 1's illuminated power LED pulls D2 LOW and updates V1 to ON after two
   seconds of stable samples; PC 2 uses D4 and V7. An unlit LED lets the input
   rise HIGH and updates only that PC to OFF/asleep. Sense changes must not
   activate either control output or increment either press count.

Hardware timing, network recovery, and actual PC startup require a configured
ESP32 and optocoupler board; compilation alone does not validate those behaviors.

References: [Blynk virtual pins](https://docs.blynk.io/en/getting-started/using-virtual-pins-to-control-physical-devices),
[Blynk connection management](https://docs.blynk.io/en/blynk-library-firmware-api/connection-management),
[ESP32 Wi-Fi API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html).
