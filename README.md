# Pocket WiFi Spy

Pocket WiFi Spy is a standalone firmware for the LilyGO T-Dongle-S3. It passively monitors nearby 2.4 GHz Wi-Fi activity and presents a simple handheld interface with SD logging and PCAP capture.

This project is built on Arduino + PlatformIO and is intended to be easy to:

- build from source
- flash directly from a prebuilt firmware image
- upload as a standalone GitHub repository

## Hardware

- LilyGO T-Dongle-S3
- ESP32-S3
- integrated ST7735 display
- integrated APA102 RGB LED
- microSD card for logs and PCAP files

## Features

- on-device display with mode and alert state
- targeted ESSID probe monitoring
- all-probe monitoring
- WPA EAPOL 4-way handshake capture
- broad deauthentication attack detection across channels 1 to 13
- SD card text logging
- SD card PCAP capture
- BOOT-button mode switching
- bundled final merged firmware image for flash-only installs


## Modes

The BOOT button cycles:

`SCOPE PROBES -> ALL PROBES -> EAPOL STEAL -> DEAUTH SCAN -> STANDBY -> SCOPE PROBES`

Mode changes show a short `LOADING...` screen before the radio stack is reconfigured.

### SCOPE PROBES

- Reads `/targets.txt` from the SD card root
- Only logs directed probe requests that exactly match one of the listed ESSIDs
- Writes text log and PCAP
- LED color: teal

If `targets.txt` is missing or empty, this mode automatically falls back to `ALL PROBES`.

### ALL PROBES

- Ignores `targets.txt`
- Logs all probe request ESSIDs seen while channel hopping
- Writes text log and PCAP
- LED color: blue

### EAPOL STEAL

- Captures WPA/WPA2 EAPOL handshake frames
- Learns target channels from `targets.txt`
- Writes text log and PCAP
- LED color: magenta

If `targets.txt` is missing or empty, the capture mode is skipped in the mode cycle and the device advances to `DEAUTH SCAN`.

### DEAUTH SCAN

- Scans channels 1 through 13 for deauthentication frames from any source in range
- Does not use `targets.txt`
- Intended to answer: "is there a deauth attack happening anywhere near me right now?"
- Uses throttled logging so only a few representative frames are written while an attack is active
- Writes text log and PCAP
- LED color: green in clear state, blinking red during an active alert

### STANDBY

- Disables promiscuous monitoring
- Keeps the UI active
- Writes text log only
- LED color: orange

### ERROR STATE

- Indicates radio or monitor configuration failure
- LED color: violet

## Display Behavior

- Top bar title: `wifi spy`
- Current mode shown in the main headline area
- In `DEAUTH SCAN`, the display switches to an alert banner when an attack is detected
- During mode changes, the display shows `LOADING...`

## SD Card Logging

If a card is present, the firmware creates a session directory and then one subdirectory per activated mode:

```text
/logs/pocket_wifi_spy_session_<boot-ms>_<session-id>/
  scope_probes/
    pocket_wifi_spy_<mode-activation-ms>_scope_probes.txt
    pocket_wifi_spy_<mode-activation-ms>_scope_probes.pcap
  all_probes/
    pocket_wifi_spy_<mode-activation-ms>_all_probes.txt
    pocket_wifi_spy_<mode-activation-ms>_all_probes.pcap
  capture/
    pocket_wifi_spy_<mode-activation-ms>_capture.txt
    pocket_wifi_spy_<mode-activation-ms>_capture.pcap
  deauth/
    pocket_wifi_spy_<mode-activation-ms>_deauth.txt
    pocket_wifi_spy_<mode-activation-ms>_deauth.pcap
  standby/
    pocket_wifi_spy_<mode-activation-ms>_standby.txt
```

Logging summary:

- `SCOPE PROBES`: probe requests
- `ALL PROBES`: probe requests
- `EAPOL STEAL`: EAPOL frames
- `DEAUTH SCAN`: representative deauth frames during an attack window
- `STANDBY`: text log only

## targets.txt

Create `/targets.txt` on the microSD card root.

Rules:

- one ESSID per line
- blank lines are ignored
- lines beginning with `#` are ignored
- matching is exact

Example:

```text
# Monitor these ESSIDs in SCOPE PROBES and EAPOL STEAL
guestnet
OfficeNet
Camera-Setup
```

An example file is included as `targets.txt.example` in the repository.

## Repository Layout

Main firmware source:

- `examples/pocket_wifi_spy/pocket_wifi_spy.ino`
- `examples/pocket_wifi_spy/esp_lcd_st7735.c`
- `examples/pocket_wifi_spy/esp_lcd_st7735.h`

PlatformIO config:

- `platformio_pocket_wifi_spy.ini`

Scripts:

- `build_pocket_wifi_spy.bat`
- `flash_pocket_wifi_spy.bat`
- `package_pocket_wifi_spy.bat`
- `build_and_flash_pocket_wifi_spy.bat`
- matching PowerShell scripts under `scripts/`

Release firmware:

- `release/pocket-wifi-spy-T-Dongle-S3-*-merged.bin`

## Dependencies

### For building from source

Install one of the following:

1. PlatformIO CLI
2. VS Code with the PlatformIO extension

Typical CLI install options:

```powershell
pip install platformio
```

or install PlatformIO through VS Code and let it manage its own runtime.

### For flashing the bundled final firmware only

Install one of the following:

1. PlatformIO
2. Python 3 plus `esptool`

If you use plain Python:

```powershell
pip install esptool
```

## Build From Source

From the repository root:

```powershell
.\scripts\build_pocket_wifi_spy.ps1
```

or:

```bat
build_pocket_wifi_spy.bat
```

This compiles the firmware and leaves build artifacts in `.pio/build/T-Dongle-S3/`.

## Build And Flash From Source

Use the dedicated build-and-flash wrapper when you want a fresh compile followed by upload:

```powershell
.\scripts\build_and_flash_pocket_wifi_spy.ps1 -Port COM16
```

or:

```bat
build_and_flash_pocket_wifi_spy.bat
```

## Flash The Bundled Final Firmware Image

If you only want to download the repo and flash the final release image without compiling, use:

```powershell
.\scripts\flash_pocket_wifi_spy.ps1 -Port COM16
```

or:

```bat
flash_pocket_wifi_spy.bat
```

If a bundled merged firmware image exists in `release/`, the flash script uses it directly and does not trigger a source build.

You can also explicitly pass a merged image path:

```powershell
.\scripts\flash_pocket_wifi_spy.ps1 -Port COM16 -ImagePath .\release\pocket-wifi-spy-T-Dongle-S3-final-merged.bin
```

## Create A New Merged Firmware Image

```powershell
.\scripts\package_pocket_wifi_spy.ps1
```

This builds the project and creates a merged image in `release/` along with a text manifest.

## Manual Flash Offsets

When flashing separate binaries manually, the offsets are:

- `0x0000` bootloader
- `0x8000` partitions
- `0x10000` firmware

Example:

```powershell
python -m esptool --chip esp32s3 --port COM16 --baud 921600 write_flash -z --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0000 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
```

## Enter Download Mode Manually

If the board does not auto-enter download mode during flashing:

1. Unplug the T-Dongle-S3.
2. Hold the BOOT button.
3. Plug the dongle in while still holding BOOT.
4. Start the flash command.
5. Release BOOT once the host detects the serial/download device.

## Notes

- This firmware monitors 2.4 GHz channels 1 through 13.
- `DEAUTH SCAN` is broad but still channel-hopped, so very short bursts can still be missed while listening on another channel.
- `SCOPE PROBES` and `EAPOL STEAL` depend on a valid `targets.txt`.
- `DEAUTH SCAN` intentionally ignores `targets.txt`.
