# Blight: Bias Lighting for Wayland

Real-time bias lighting system for Linux Wayland using ESP32 + WS2812B LEDs. Captures screen edges via PipeWire (using XDG Desktop Portal) and sends RGB data to ESP32 over WiFi.

## Overview

Uses `libportal` and `PipeWire` to capture screen borders, downsamples to an LED grid, and transmits color data to an ESP32 via UDP. Optimized for minimal CPU usage and low latency by accessing native PipeWire buffers.

**Workflow:**
1. Request screencast via XDG Desktop Portal (`libportal`). (skipped in `static` mode)
2. Capture buffers directly from PipeWire (supports DMA-BUF and MemFd).
3. Sample edges (Left, Top, Right), average colors in linear light, and re-encode to sRGB.
4. Transmit RGB data to ESP32 over WiFi/UDP.

## Hardware

- ESP32 with WiFi
- WS2812B LED strip (connected to GPIO 14)
- USB-C power supply
- Recommended setup: ~62 LEDs for a standard monitor

## Build

### Prerequisites
Ensure you have the following development libraries installed:
- `libportal`
- `glib-2.0`
- `libpipewire-0.3`
- `pkg-config`
- `gcc`
- `make`

### Compilation
Simply run:
```bash
make
```
This builds the `blight` binary. The default configuration uses WiFi communication.

## Usage

```bash
./blight live               # stream screen edge colors to the LEDs
./blight static RRGGBB      # set a fixed color, no screen capture
```

**Modes:**

- `live`: captures the screen via PipeWire and drives the LEDs from the average of the screen edges. It averages in perceptual (gamma-encoded) space and applies a fixed vibrance lift (no user-facing saturation knob), uses a fixed brightness of `250`, and the ESP32 eases toward each new value smoothly.
- `static`: sends a single fixed color and exits. No screencast permission or PipeWire connection is used, so there is zero capture overhead. The ESP32 stays on that color indefinitely (it does not time out) until a `live` frame arrives. Accepts 3- or 6-digit hex with an optional `#` (`FFFFFF`, `#fff`, `2a1a4d`).

**Examples:**

```bash
./blight live
./blight static FFFFFF
./blight static 2a1a4d
```

## ESP32 Setup

1. **Credentials:** Create `esp32/main/creds.h` with your WiFi info:
   ```cpp
   #define WIFI_SSID "YourSSID"
   #define WIFI_PASS "YourPassword"
   ```
2. **Configuration:** Adjust `NUM_LEDS` and `DATA_PIN` in `esp32/main/main.ino` if necessary. Light quality is fixed there via `LED_BRIGHTNESS` (250) and the smoothing time constant `LED_SMOOTHING_TAU_MS`.
3. **Flash:** Use Arduino IDE or `arduino-cli` to flash the ESP32.
4. **Network:** The host app expects the ESP32 at `192.168.1.100` (static IP) by default. You can change this in `src/main.c`.

## Performance Note

Host CPU usage is low (around 3% of a single core on a 1440p capture). The
expensive part of screencasting is reading edge pixels out of the (typically
uncached) PipeWire/DMA-BUF frame, so the sampler is deliberately resolution
independent: each edge box is sampled at an adaptive stride that keeps the
number of pixels read per box roughly constant (512 by default) no matter how
large the screen is, and each pixel is read with a single 32-bit load instead of
three separate channel loads. Set `BLIGHT_SAMPLES` to a higher value for more
accurate averages (more CPU) or lower for less. Note that any PipeWire
screencast may still cause minor FPS drops in some Wayland compositors
(like GNOME/Mutter) due to how they handle buffer synchronization. `static`
mode skips capture entirely, so it has effectively zero runtime overhead once set.

## TODO

- [ ] **Daemon + Control Tool**: Implement `blightd` daemon with `blightctl` for runtime control.
- [ ] **XDG Portal Token Restoration**: Save authorization token to avoid permission dialogs on startup.
- [ ] **Black Boundary Detection**: Automatically skip black bars (letterboxing) for different aspect ratios.
