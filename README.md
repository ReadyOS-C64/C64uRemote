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

## Planned Next Version

A more capable follow-up version is planned for the M5Dial.

The idea for that version is to use:

- the dial itself for CPU speed control
- touchscreen buttons for common actions
- stored macros for repeated workflows such as loading a config, running a `.d64`, loading a binary into REU, and then launching an app
- remembering recently loaded items and common last-used targets
- richer navigation of the C64 Ultimate file and menu system so loading content directly from the handheld becomes practical

## License

MIT. See [LICENSE](LICENSE).
