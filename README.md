# Pocket Nova

A pocket remote, presentation clicker and virtual pet for the **M5Stack Atom Matrix** (ESP32, 5×5 LED screen, one button, tilt sensor, IR LED).

**Website and guide:** https://unorfl.github.io/PocketNova/
**Phone remote:** https://unorfl.github.io/PocketNova/remote.html

## Features

- **Nova, the pet.** Reacts to taps, tilts, shaking and rocking. Sleeps at night and remembers how much you like it.
- **11 apps.** Media remote, universal TV remote (infrared, 20 brands), slide clicker, Windows shortcuts, spirit level, dice, timer, mood lights, torch, Snake and settings.
- **Bluetooth keyboard.** 3 device slots. On Windows, a "Connect" pop-up appears while it's ready to pair (Swift Pair).
- **Wi-Fi.** Its own setup network with a sign-in page that opens by itself. Gets the time from the internet.
- **Phone remote.** A web page that controls it over Bluetooth, protected by a code shown on the LEDs.
- **PC panel (Windows).** Opens by itself when Pocket Nova is plugged in. Every setting, a live copy of the screen and firmware updates.

## Download

**[Download the latest release](https://github.com/UnoRfl/PocketNova/releases/latest)**

| File | What it is |
|---|---|
| `PocketNova-firmware.bin` | The firmware. Flash it at address `0x0`. |
| `PocketNova-Panel.zip` | The Windows control panel (needs Python 3). |

### Install the firmware

1. Plug the Atom Matrix into a computer with a USB-C cable that carries data.
2. Open [Espressif's web flasher](https://espressif.github.io/esptool-js/) in Chrome or Edge and click **Connect**.
3. Set the address to `0x0`, choose `PocketNova-firmware.bin` and click **Program**.

Or with esptool:

```
esptool --chip esp32 --port COM3 write_flash 0x0 PocketNova-firmware.bin
```

Flashing the full file resets saved settings and Bluetooth pairings.

### Install the PC panel

Unzip `PocketNova-Panel.zip`, right-click `install.ps1` and choose **Run with PowerShell**. Run `uninstall.ps1` to remove it.

### Phone remote

Open the [remote page](https://unorfl.github.io/PocketNova/remote.html) in Chrome on Android, tap **Connect** and type the code that scrolls on the LEDs. On iPhone, use the Bluefy browser (Safari has no Web Bluetooth).

## Build from source

Arduino ESP32 core 2.0.17, FastLED 3.6.0, M5Atom 0.1.3, IRremoteESP8266 2.9.0, ArduinoJson 7.

```
cd firmware
arduino-cli compile --fqbn "esp32:esp32:m5stack-atom:PartitionScheme=min_spiffs" PocketNova
arduino-cli upload -p COM3 --fqbn "esp32:esp32:m5stack-atom:PartitionScheme=min_spiffs,UploadSpeed=115200" PocketNova
```

TV codes live in `firmware/tools/tv_codes.json`. Run `gen_tvcodes.py` to rebuild `TvCodes.h`.

## Project layout

| Folder | Contents |
|---|---|
| `firmware/PocketNova` | The firmware (Arduino sketch) |
| `firmware/tools` | TV code data and generator |
| `pc` | Windows control panel |
| `docs` | Website: guide and phone remote |

## Credits

- The Bluetooth keyboard code (`NovaKeyboard.*`) is based on [ESP32-BLE-Keyboard](https://github.com/T-vK/ESP32-BLE-Keyboard) by T-vK, with fixes.
- TV codes were checked against Flipper-IRDB, the LIRC remotes database, probonopd/irdb, SmartIR and TV-B-Gone. Sources are listed in `tv_codes.json`.
