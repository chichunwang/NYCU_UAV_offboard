#!/usr/bin/env bash

set -Eeuo pipefail

readonly RUNTIME_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck source=common.sh
source "${RUNTIME_DIR}/common.sh"

load_config
wait_for_serial_device "Pixhawk" "$PIXHAWK_SERIAL"

dds_agent="$DDS_AGENT_BINARY"
if [[ "$dds_agent" == */* ]]; then
    [[ -x "$dds_agent" ]] || die "DDS Agent is not executable: ${dds_agent}"
else
    dds_agent="$(command -v "$dds_agent" || true)"
    [[ -n "$dds_agent" ]] ||
        die "DDS Agent '${DDS_AGENT_BINARY}' was not found in PATH"
fi

log "Starting Micro XRCE-DDS Agent on ${PIXHAWK_SERIAL} at ${PIXHAWK_BAUD} baud"
exec "$dds_agent" serial --dev "$PIXHAWK_SERIAL" -b "$PIXHAWK_BAUD"

