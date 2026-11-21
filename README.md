# MCZ Oven Local Bridge

The goal of this project is to provide local access to *your* oven without relying on the MCZ cloud. It acts as a bridge that connects to your oven's display via Bluetooth and forwards the data over Wi-Fi to an MQTT server.

Home Assistant integration is included — all entities are discovered automatically, so no separate integration is required.

## Build Targets

There are two build targets:

**Generic ESP32** (Bluetooth ↔ Wi-Fi bridge):

```bash
platformio run -e esp32dev
```

**CYD display:**

```bash
platformio run -e cyd
```

> **Note:** You need to copy `config_example.h` to `config.h` first.

More information about the CYD can be found at:
https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display

## Upload

```bash
platformio run -e <target> --target upload --upload-port "$PORT"
```

## Home Assistant entities

Discovered automatically over MQTT (some only if the oven has the circuit: boiler, buffer, hydro settings).

- **Climate and controls**: `Stove` (off / heat, preset = oven mode: Comfort, Overnight, Turbo, Auto, Manual), `Power`, `Fan` / `Fan 2` / `Fan 3` (auto or level), `Silent` switch.
- **Oven sensors**: flue gas, control board, room, outdoor and panel temperatures, boiler and buffer temperatures (hydro), flue gas fan and auger setpoint/speed, air pressure, actual power, `Phase` (with `fase_op` / `sub_fase_op` attributes), pellet autonomy. Hydro settings (hysteresis, pump min. temperature, water setpoint) are writable `number` entities. Backup-battery diagnostics are disabled by default.
- **Counters**: ignitions, total working time, time per power level (1 to 5), hours to service. A guard rejects out-of-range values so that Home Assistant statistics are not polluted (`PATCHES.md` section 8).
- **Alarms**: `Alarm` (live, with code and hint), `Last alarm`, `Alarm history`, and an `Acknowledge alarm` button.
- **Combustion recipes**: `Correction pellets` and `Correction active` (`number`, -3..+3, writable), the raw values (`Recette tirage (brut)`, pellet, flow) and the recipe indexes of register 867, and a read-only `Mode adaptatif` binary sensor. The value shown in the MCZ app (-3..+3) is the raw register minus 4 (checked on hardware). The air recipe (0x0578) is read only here, the app does not expose it. Writes are guarded: the raw value read must be in 1..7 and recent, one write per 10 s and per register, and the value read back is what gets published. Each write probably goes to the oven's non-volatile memory, so do not loop on it. `RECIPE_WRITE 0` in `config.h` disables writing. **The oven display only accepts one BLE connection: with the MCZ app open on a phone, the bridge cannot connect.** Details: `PATCHES.md` section 18.
- **Bridge diagnostics**: IP, OTA state, uptime, Wi-Fi reconnections, WiFi signal (RSSI), boot reason, boot history and boot count, oven reset cause, `Diag pont` (`esp32dev_diag` build only, `PATCHES.md` section 12).

## Wi-Fi and BLE coexistence

The ESP32 has a single radio shared by Wi-Fi and BLE. The bridge gives Wi-Fi priority: BLE is silenced while Wi-Fi associates, the BLE scan runs at a reduced duty cycle, the BLE connect timeout is 5 s, modem sleep stays on (required with BLE), and a watchdog redoes a full association after 60 s without Wi-Fi and reboots after 10 min (`PATCHES.md` section 16). Wi-Fi transmit power is limited to 8.5 dBm by default, which avoids current peaks that a weak supply (for instance another ESP's output) cannot sustain; set `WIFI_TX_POWER` in `config.h` to raise or lower it (`PATCHES.md` section 17). If Wi-Fi is not up after `FALLBACK_AP_DELAY_S` seconds, a fallback access point opens (`PATCHES.md` section 7).

## Boot history

The last 10 reset reasons (`poweron`, `brownout`, `panic`, `task_wdt`, `sw`...) and a boot counter are kept in NVS (one write per boot), so a boot loop is visible afterwards even after rewiring the ESP. They are shown on the serial console at boot and exposed as `boot_history` / `boot_count` (`PATCHES.md` section 19).

## Docker build tools

`tools/docker/` builds with PlatformIO inside Docker (nothing to install on the host):
`tools/docker/build.sh <env>` (esp32dev, esp32dev_diag, cyd); toolchains are cached in the
Docker volume `pio-cache`. No upload/flash.

## Over-the-air update (HTTP)

The ESP serves `POST /update` (multipart, field `firmware`, Basic Auth `admin` / `OTA_PASSWORD`
from `src/config.h`; absent or empty = OTA disabled). From the Mac, with nothing installed
besides Docker:

```bash
tools/docker/ota.sh <ip> [env]    # builds in Docker, then curl; password from $OTA_PASSWORD or prompt
```

The Mac must be able to reach the ESP: same LAN, or, if the ESP is on its fallback access
point, the Mac joined to that access point (use the IP printed on the serial console or by `GET /`).
`GET /` (no auth) returns a short status page: build date, IP, uptime, OTA state.

The first flash has to be over USB (browser, esptool-js); OTA also needs a partition table with
two OTA slots: `esp32dev` / `esp32dev_diag` use `default.csv`, `cyd` keeps `huge_app.csv` and has no OTA. Files, offsets and caveats: see `PATCHES.md` section 14.

## Serial commands

115200 baud, type `help` for the full list. Most useful:

- `status`, `poll`, `wifi` (network state and visible networks), `scan`, `target <mac|none>`
- `temp <c>`, `power <1-5>`, `mode <0-4>`, `fan|fan2|fan3 <auto|1-5>`, `silent <on|off>`, `on`, `off`
- `alarms`, `ack`, `settime`, `getchrono`
- `recipes [read]` (raw recipe values, indexes, age of the last read), `boots [clear]` (boot history)
- `r <regHex> <count>`, `w <regHex> <valHex>`, `wm <regHex> <v..>` (raw register access), `log <on|off>`

## More details

`PATCHES.md` lists every correction made to this fork, section by section, with the symptom, the
cause and what remains to be validated. `docs/` holds the official Maestro M2 register table
(`mcz_m2_registers.csv`, `mcz_m2_registers_en.csv`).

## Disabling the Cloud Connection

With this solution in place, you can disable the oven's Wi-Fi connection entirely. If you prefer to block the connection at your router instead, note that the oven tries to reach the hostname *m.maestro.mcz.it*.

## Pairing

For the Bluetooth connection to work, the oven display must be in pairing mode. Press **+** and **−** simultaneously to toggle Bluetooth off and on. Alternatively, disconnect the oven from power for a few seconds — after a reboot, the display is also in pairing mode.

## Remote Control

Perhaps someone can provide the firmware and some photos of the inside of the MCZ remote control (4022002). We would like to understand how the external room thermostat works.

The pinout of the ESP32 inside the Maestro M2 oven display is as follows:

* unused (J1)
* 3.3 V VCC
* TX
* RX
* EN
* IO0
* unused
* GND

To flash it, IO0 must be pulled to GND during reset.

## Possible Future Projects

With this codebase, it would be possible to replace the oven display entirely — the display and the oven mainboard speak the same protocol.