# Hardware notes

Waveshare ESP32-S3-Knob-Touch-LCD-1.8. Bought August 2026 for £52.79 from a third-party Amazon
listing without the battery, because every official channel was sold out at the time.

**Everything here came from Waveshare's own demo sources and wiki**, downloaded from
`files.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8/`, and then checked against the board in
hand. Nothing is from a third-party README, and that distinction matters: a well-regarded
third-party driver README for a different display in this family lists SCK 39 / MOSI 38, and
both of those pins are unconnected. The real values were in the vendor's own `user_config.h`.

**Board variants differ.** Reviews of the reseller listings describe inconsistencies, and the
vendor's own `07_Audio_Test/user_config.h` carries commented-out alternate pins for the I2S
*output* (`48/38/47` beside the live `39/40/41`), which reads like a board revision. The PDM
microphone pins carry no alternates, so the capture path this project depends on looks stable.
Verify against the board in your hand rather than trusting this file.

## THE GOTCHA THAT WILL EAT AN HOUR: two MCUs, one USB-C port

This board has **two chips - an ESP32-S3 and a plain ESP32** - and a CH445P analog switch
wired so that **the two orientations of the USB-C plug reach different download channels.**
Waveshare's own FAQ: if flashing fails or no serial port appears, unplug, **rotate the plug
180 degrees**, and try again. Nothing is broken.

Our firmware targets the **ESP32-S3** (mic, SD, display, WiFi all hang off it).

A buyer review of one reseller listing also reports that **battery tabs can be wired
backwards relative to the PCB's + / - print** - meter before connecting anything, per the
existing safety rule.

## The dial is not a quadrature encoder

Each contact is an independent direction switch, polled every 3ms with a two-sample debounce,
firing on release. Treating it as a quadrature encoder costs a day and produces a dial that
appears to turn one way only.

This firmware already implements it correctly. The full explanation, the failure symptoms and
a working implementation are in **[THE-DIAL.md](THE-DIAL.md)**, which is worth reading before
you build anything else on this board.

### The two MCUs, in more detail

The USB-C plug orientation really does switch which MCU the port reaches - S3 native USB
(VID 303A:1001) one way, CH340 UART bridge (VID 1A86:7523) the other. The board has a
second encoder, EC2 (IO19/IO22), wired to the ESP32. Factory images for both chips are in
the wiki's BIN zip and flash at 0x0; both were restored after testing.

## The microSD slot is INTERNAL, and the factory card died (14 Aug)

The board ships with a small test card already seated - ours reported itself as
"APPSD, 480MB", and an Amazon reviewer of the same listing found a 512MB card in theirs
too. **The slot is inside the case**, so the card is invisible without disassembly and
cannot be knocked loose by handling.

That card worked for several hours (mic test recorded and uploaded through it), then
failed permanently mid-session: `sdmmc_init_ocr: send_op_cond returned 0x107` on every
retry. A recording of 4,618,240 bytes was written and closed cleanly just before, and the
directory was empty afterwards - the audio was written to a card that never committed its
metadata.

**Fit a real card before trusting this device with anything.** Any decent 32GB microSD is
~GBP 7 and takes capacity from 4.5 hours to ~290. Requires opening the case, which is the
same job as fitting a battery.

Firmware now treats a missing card as a first-class state: full amber screen, tap refused
with a buzz, and a retry every 5s so a reseated or replaced card just starts working.

## Verified pin map (vendor sources)

| Function | Pins | Source file |
|---|---|---|
| **PDM microphone** | DATA **GPIO46**, CLK **GPIO45** | `07_Audio_Test/user_config.h` |
| I2S audio out (PCM5100A DAC) | BCLK 39, WS 40, DOUT 41 | same (alternates 48/38/47 commented) |
| Rotary encoder (S3 side) | A **GPIO8**, B **GPIO7** | same |
| **microSD (SDMMC, 4-bit)** | CLK 4, CMD 3, D0 5, D1 6, D2 42, D3 2 | `02_SD_Card/sd_card_bsp.cpp` |
| Display (QSPI) | CS 14, PCLK 13, D0 15, D1 16, D2 17, D3 18, RST 21, backlight 47 | `08_LVGL_Test/lcd_config.h` |
| Touch (I2C) | SDA 11, SCL 12, addr 0x15 | same |

Display is **360x360, SH8601 controller** (`esp_lcd_sh8601.c` ships in the vendor demo).
Also onboard: DRV2605 vibration driver (I2C), 3.5mm jack, MX1.25 battery socket, power
button, ceramic antenna. Battery ADC pin not identified in the demos - measure on arrival.

## Why PlatformIO and not ESPHome

If you are wondering whether this board could be an ESPHome device instead, it was checked
rather than assumed:

- **ESPHome 2026.7.3 has no SH8601 driver.** Checked in the installed component tree, not from
  memory: `qspi_dbi` supports RM67162, RM690B0, AXS15231, JC4832W535, JC3636W518, and
  `mipi_spi`'s Waveshare models cover CO5300, ST7789 variants and AXS15231. No SH8601, and no
  model entry for this board. An ESPHome build means authoring a display driver first.
- ESPHome also has no path for PDM-mic-to-SD WAV recording, which is the entire device.
- The vendor's demo already proves display, PDM mic, SD and encoder on this exact hardware.

So: PlatformIO with the Arduino framework. Note the `platformio.ini` in this repo does **not**
use the stock `espressif32` platform, because that ships Arduino core 2.0.17, whose legacy
`driver/i2s.h` has no `i2s_pdm.h`. The vendor demo is written against core 3.x, so the build
uses the pioarduino continuation instead of rewriting vendor code into a deprecated API.

## What the vendor demo does and does not prove

`07_Audio_Test` runs PDM RX at **44.1kHz mono 16-bit** and loops the mic to the speaker. It
proves the microphone exists and works; it does **not** record to a file. The capture path this
firmware uses, PDM RX at 16kHz straight to a WAV on the SD card, had to be written from
scratch.

## Sources

- Wiki: `https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8`
- Demo: `https://files.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8/ESP32-S3-Knob-Touch-LCD-1.8-Demo.zip` (66MB)
- Schematic: same path, `-schematic.zip` - pull this if any pin above is contradicted
