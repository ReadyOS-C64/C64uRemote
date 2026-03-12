# C64 Ultimate Remote Using M5Stack M5Stick

Small handheld remote firmware for the M5Stack M5StickC Plus2 that controls a C64 Ultimate over its REST API.

This project is part of the ReadyOS project:
https://readyos.notion.site

## Features

- Full-screen Commodore logo home screen with demo-style effects
- Soft reset and hard reset actions for the C64 Ultimate
- C64 Ultimate on-screen menu trigger from the device
- CPU speed read and set support through the REST API
- Connection and authentication test screen
- Status screen for Wi-Fi, target reachability, auth state, host, and CPU speed
- Simple button-driven UI designed for the M5StickC Plus2 screen

## Configuration

Create a local `.env` file in the project root using `.env.example` as the starting point.

Example:

```env
C64U_WIFI_SSID=YOUR_WIFI_SSID
C64U_WIFI_PASSWORD=YOUR_WIFI_PASSWORD
C64U_TARGET_HOST=10.0.0.9
C64U_TARGET_PASSWORD=karl
```

The deploy script reads `.env` and generates `src/build_env.h` locally at build time. `.env` and `src/build_env.h` are ignored and should not be committed.

## Build and Flash

```bash
./scripts/setup.sh
./scripts/deploy.sh
./scripts/deploy.sh --build-only
./scripts/deploy.sh --port /dev/cu.usbserial-XXXX
```

## Hardware / Software

- M5Stack M5StickC Plus2
- PlatformIO
- VS Code
- M5Unified

## License

MIT. See [LICENSE](LICENSE).
