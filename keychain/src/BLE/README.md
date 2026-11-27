# BLE library (copy)

The ESP32 Arduino core 3.3.4 BLE library, copied here with one fix in
`BLECharacteristic.cpp` (search `KEYCHAIN FIX`): `onWrite` runs before the
write is acknowledged, as in core 3.3.12. Without it, fast writes (firmware
updates, drawings, song names) were lost or doubled.

When the PC's core is 3.3.12 or newer, this folder can go: switch the
`#include "src/BLE/..."` lines in keychain.ino back to `<BLEDevice.h>` etc.
