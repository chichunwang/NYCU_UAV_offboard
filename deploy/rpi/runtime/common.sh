#!/usr/bin/env bash

set -Eeuo pipefail

readonly NYCU_CONFIG_FILE="/etc/nycu-uav-offboard/config.env"

log()
{
    printf '[nycu-uav-offboard] %s\n' "$*" >&2
}

die()
{
    log "ERROR: $*"
    exit 1
}

require_variable()
{
    local name="$1"
    [[ -n "${!name:-}" ]] || die "Missing ${name} in ${NYCU_CONFIG_FILE}"
}

require_unsigned_integer()
{
    local name="$1"
    local value="${!name:-}"
    [[ "$value" =~ ^(0|[1-9][0-9]*)$ ]] ||
        die "${name} must be an unsigned integer, got '${value}'"
}

load_config()
{
    [[ -r "$NYCU_CONFIG_FILE" ]] ||
        die "Cannot read ${NYCU_CONFIG_FILE}; run the RPi installer first"

    # The file is root-owned and generated with shell-escaped values.
    # shellcheck disable=SC1090
    source "$NYCU_CONFIG_FILE"

    local required=(
        NYCU_SERVICE_USER
        NYCU_USER_HOME
        NYCU_ROS_WS
        ROS_SETUP
        ROS_DOMAIN_ID
        PIXHAWK_SERIAL
        PIXHAWK_BAUD
        LR24_SERIAL
        LR24_BAUD
        DDS_AGENT_BINARY
        DEVICE_WAIT_TIMEOUT_SECONDS
    )
    local name
    for name in "${required[@]}"; do
        require_variable "$name"
    done

    require_unsigned_integer ROS_DOMAIN_ID
    require_unsigned_integer PIXHAWK_BAUD
    require_unsigned_integer LR24_BAUD
    require_unsigned_integer DEVICE_WAIT_TIMEOUT_SECONDS

    (( ROS_DOMAIN_ID <= 232 )) ||
        die "ROS_DOMAIN_ID must be between 0 and 232"
    (( PIXHAWK_BAUD > 0 )) || die "PIXHAWK_BAUD must be greater than zero"
    case "$LR24_BAUD" in
        9600|19200|38400|57600|115200|230400)
            ;;
        *)
            die "Unsupported LR24_BAUD: ${LR24_BAUD}"
            ;;
    esac

    local running_user
    running_user="$(id -un)"
    [[ "$running_user" == "$NYCU_SERVICE_USER" ]] ||
        die "Service is running as '${running_user}', expected '${NYCU_SERVICE_USER}'"

    export HOME="$NYCU_USER_HOME"
    export ROS_DOMAIN_ID
}

wait_for_serial_device()
{
    local label="$1"
    local device="$2"
    local timeout_seconds="$DEVICE_WAIT_TIMEOUT_SECONDS"
    local elapsed=0

    while [[ ! -c "$device" || ! -r "$device" || ! -w "$device" ]]; do
        if (( timeout_seconds > 0 && elapsed >= timeout_seconds )); then
            die "Timed out waiting for readable/writable ${label} device: ${device}"
        fi

        if (( elapsed == 0 || elapsed % 60 == 0 )); then
            log "Waiting for readable/writable ${label} device: ${device}"
        fi

        sleep 1
        elapsed=$((elapsed + 1))
    done

    log "${label} device is ready: ${device}"
}
