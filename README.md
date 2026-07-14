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