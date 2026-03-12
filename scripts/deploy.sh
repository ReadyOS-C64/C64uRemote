#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)"
APP_DIR="$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)"
SETUP_SCRIPT="$SCRIPT_DIR/setup.sh"
ENV_FILE="$APP_DIR/.env"
BUILD_ENV_HEADER="$APP_DIR/src/build_env.h"
DEFAULT_ENV="m5stack-stickc-plus2"
MONITOR=0
BUILD_ONLY=0
PORT=""
ENV_NAME="${PIO_ENV:-$DEFAULT_ENV}"

usage() {
  cat <<'EOF'
Usage: ./scripts/deploy.sh [--port /dev/cu.xxx] [--env name] [--build-only] [--monitor]

Options:
  --port        Serial port to use for upload.
  --env         PlatformIO environment name. Default: m5stack-stickc-plus2
  --build-only  Compile only, do not upload.
  --monitor     Open a serial monitor after upload.
  --help        Show this message.
EOF
}

detect_port() {
  local port
  local candidates=()

  shopt -s nullglob
  candidates=(
    /dev/cu.usbmodem*
    /dev/cu.usbserial*
    /dev/cu.wchusbserial*
    /dev/cu.SLAB_USBtoUART*
    /dev/cu.serial*
  )
  shopt -u nullglob

  for port in "${candidates[@]:-}"; do
    if [[ -e "$port" ]]; then
      printf '%s\n' "$port"
      return 0
    fi
  done

  return 1
}

escape_c_string() {
  local value="$1"
  value="${value//\\/\\\\}"
  value="${value//\"/\\\"}"
  printf '%s' "$value"
}

generate_build_header() {
  local ssid="${C64U_WIFI_SSID:-}"
  local wifi_password="${C64U_WIFI_PASSWORD:-}"
  local target_host="${C64U_TARGET_HOST:-10.0.0.9}"
  local target_password="${C64U_TARGET_PASSWORD:-karl}"

  cat >"$BUILD_ENV_HEADER" <<EOF
#pragma once
#define C64U_WIFI_SSID "$(escape_c_string "$ssid")"
#define C64U_WIFI_PASSWORD "$(escape_c_string "$wifi_password")"
#define C64U_TARGET_HOST "$(escape_c_string "$target_host")"
#define C64U_TARGET_PASSWORD "$(escape_c_string "$target_password")"
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)
      PORT="${2:-}"
      shift 2
      ;;
    --env)
      ENV_NAME="${2:-}"
      shift 2
      ;;
    --build-only)
      BUILD_ONLY=1
      shift
      ;;
    --monitor)
      MONITOR=1
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -f "$ENV_FILE" ]]; then
  set -a
  # shellcheck disable=SC1090
  source "$ENV_FILE"
  set +a
else
  echo "No .env found at $ENV_FILE; using empty Wi-Fi defaults and built-in target defaults."
fi

generate_build_header

if [[ ! -x "$APP_DIR/.venv/bin/pio" ]]; then
  "$SETUP_SCRIPT"
fi

PIO="$APP_DIR/.venv/bin/pio"

if [[ ! -x "$PIO" ]]; then
  echo "PlatformIO CLI was not found after setup." >&2
  exit 1
fi

if [[ "$BUILD_ONLY" -eq 0 && -z "$PORT" ]]; then
  if ! PORT="$(detect_port)"; then
    echo "No serial port detected. Pass one explicitly with --port /dev/cu.xxx" >&2
    exit 1
  fi
fi

cd "$APP_DIR"

if [[ "$BUILD_ONLY" -eq 1 ]]; then
  echo "Building $ENV_NAME"
  "$PIO" run -e "$ENV_NAME"
  exit 0
fi

echo "Uploading $ENV_NAME to $PORT"
"$PIO" run -e "$ENV_NAME" -t upload --upload-port "$PORT"

if [[ "$MONITOR" -eq 1 ]]; then
  "$PIO" device monitor --port "$PORT" --baud 115200
fi
