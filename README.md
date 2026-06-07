# Blight: Bias Lighting for Wayland

Real-time bias lighting system for Linux Wayland using ESP32 + WS2812B LEDs. Captures screen edges via PipeWire (using XDG Desktop Portal) and sends RGB data to ESP32 over WiFi.

## Overview

Uses `libportal` and `PipeWire` to capture screen borders, downsamples to an LED grid, and transmits color data to an ESP32 via UDP. Optimized for minimal CPU usage and low latency by accessing native PipeWire buffers.

**Workflow:**
1. Request screencast via XDG Desktop Portal (`libportal`).
2. Capture buffers directly from PipeWire (supports DMA-BUF and MemFd).
3. Sample edges (Left, Top, Right) and average colors.
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
./blight [brightness] [saturation] [smoothing]
```

**Parameters:**

- `brightness`: 0-255 (default: 150)
- `saturation`: float (default: 1.0, determines color vibrancy. Try 1.5-2.0)
- `smoothing`: 0.1-1.0 (default: 1.0, lower values = smoother transitions)

**Example:**

```bash
./blight 200 1.8 0.7
```

## ESP32 Setup

1. **Credentials:** Create `esp32/main/creds.h` with your WiFi info:
   ```cpp
   #define WIFI_SSID "YourSSID"
   #define WIFI_PASS "YourPassword"
   ```
2. **Configuration:** Adjust `NUM_LEDS` and `DATA_PIN` in `esp32/main/main.ino` if necessary.
3. **Flash:** Use Arduino IDE or `arduino-cli` to flash the ESP32.
4. **Network:** The host app expects the ESP32 at `192.168.1.100` (static IP) by default. You can change this in `src/main.c`.

## Performance Note

CPU usage is minimal (<2%) as it avoids heavy processing pipelines. Note that any PipeWire screencast may cause minor FPS drops in some Wayland compositors (like GNOME/Mutter) due to how they handle buffer synchronization.

## TODO

- [ ] **Daemon + Control Tool**: Implement `blightd` daemon with `blightctl` for runtime control.
- [ ] **XDG Portal Token Restoration**: Save authorization token to avoid permission dialogs on startup.
- [ ] **Black Boundary Detection**: Automatically skip black bars (letterboxing) for different aspect ratios.
