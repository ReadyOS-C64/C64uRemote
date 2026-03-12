#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)"
APP_DIR="$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)"
VENV_DIR="$APP_DIR/.venv"

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 is required but was not found on PATH." >&2
  exit 1
fi

if [[ ! -d "$VENV_DIR" ]]; then
  echo "Creating local virtualenv at $VENV_DIR"
  python3 -m venv "$VENV_DIR"
fi

echo "Installing PlatformIO into $VENV_DIR"
"$VENV_DIR/bin/python" -m pip install --upgrade pip setuptools wheel
"$VENV_DIR/bin/python" -m pip install "platformio>=6.1,<7"

echo
echo "PlatformIO is ready:"
echo "  $VENV_DIR/bin/pio --version"
