#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
dawn_cli="${DAWN_ARDUINO_CLI:-}"
if [ -z "$dawn_cli" ]; then
    dawn_cli="$(command -v arduino-cli || true)"
fi
if [ -z "$dawn_cli" ] && [ -x '/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli' ]; then
    dawn_cli='/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli'
fi
if [ -z "$dawn_cli" ]; then
    echo 'Arduino CLI is required (or set DAWN_ARDUINO_CLI to its path).' >&2
    exit 1
fi
"$dawn_cli" compile --fqbn "${DAWN_BOARD_FQBN:-esp32:esp32:esp32s3:PSRAM=opi}" \
    --build-path "$PWD/output/build-esp32s3" \
    --output-dir "$PWD/output/esp32s3" "$PWD"
