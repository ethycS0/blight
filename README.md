# Blight: Bias Lighting for Wayland

Real-time bias lighting system for Linux Wayland using ESP32 + WS2812B LEDs. Captures screen edges via PipeWire and sends RGB data to ESP32 over WiFi or serial.

## Overview

Uses PipeWire + XDG Desktop Portal + GStreamer to capture screen borders, downsamples to LED grid resolution (160x90@24hz), and transmits color data to ESP32. Optimized for minimal CPU usage (<2%).

**Pipeline:** `pipewiresrc → queue → videorate → capsfilter → videoconvertscale → appsink`

## Hardware

- ESP32 with WiFi
- WS2812B LED strip
- USB-C PD power supply with 3A fuse
- My setup: 68 LEDs (32 bottom + 18 left + 18 right)

## Build (NixOS)

```bash
# Serial communication (default)
nix build .#blight_serial

# WiFi communication
nix build .#blight_wifi

# Development shell
nix develop
```

**Note:** GPU builds (`blight_*_gpu`) not supported this commit - VA-API causes 60% CPU usage.

## Usage

```bash
./result/bin/blight [brightness] [saturation] [smoothing]
```

**Parameters:**

- `brightness`: 0-255 (default: 150)
- `saturation`: float (default: 1.0, try 1.5-2.0 for more vibrant colors)
- `smoothing`: 0.1-1.0 (default: 1.0, lower = smoother transitions)

**Example:**

```bash
./result/bin/blight 200 1.8 0.7
```

## ESP32 Setup

1. Edit `esp32/main/creds.h` with WiFi credentials (WiFi mode)
2. Configure LED pin/count in `esp32/main/main.ino`
3. Flash to ESP32
4. **Serial:** Connects to `/dev/ttyUSB0` at 921600 baud
5. **WiFi:** Edit IP in `src/wifi.c`, ESP32 listens on port 4210

## Performance Note

CPU usage is minimal, but active PipeWire screencast causes GPU sync points on GNOME/Mutter, leading to 7-10% FPS drop in intensive games. This is a compositor limitation. KDE Plasma handles this better, or disable when gaming.

## TODO

- [ ] **Daemon + Control Tool**: Implement `blightd` daemon with `blightctl` for runtime control
  - Start/stop capture without restarting
  - Adjust brightness/saturation/smoothing on the fly
  - Unix socket IPC for low overhead
- [ ] **XDG Portal Token Restoration**: Save authorization token for persistent permissions
  - No permission dialog on every startup
  - Store in `~/.config/blight/portal_token`
  - Requires portal protocol v4+ (xdg-desktop-portal ≥1.12.1)
- [ ] **Black Boundary Detection**: Skip black bars for different aspect ratios
  - Auto-detect letterboxing (21:9 content on 16:9 display)
  - Auto-detect pillarboxing (4:3 content on 16:9 display)
  - Sample only actual video content, ignore black borders
  - Better color accuracy for non-native aspect ratio content

---
