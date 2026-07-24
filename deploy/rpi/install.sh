#!/usr/bin/env bash

set -Eeuo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd -P)"
readonly CONFIG_DIR="/etc/nycu-uav-offboard"
readonly CONFIG_FILE="${CONFIG_DIR}/config.env"
readonly RUNTIME_DEST="/usr/local/lib/nycu-uav-offboard"
readonly SYSTEMD_DEST="/etc/systemd/system"

SERVICE_USER="${SUDO_USER:-}"
WORKSPACE=""
PIXHAWK_DEVICE=""
LR24_DEVICE=""
ROS_DISTRO="jazzy"
ROS_DOMAIN_ID="0"
PIXHAWK_BAUD="921600"
LR24_BAUD="115200"
DDS_AGENT=""
DEVICE_WAIT_TIMEOUT_SECONDS="0"
START_NOW=1

usage()
{
    cat <<'EOF'
Install the NYCU UAV DDS Agent and GPS GOTO nodes as Raspberry Pi systemd services.

Usage:
  sudo bash deploy/rpi/install.sh \
    --workspace /home/pi/NYCU_ROS_WS \
    --pixhawk-device /dev/serial/by-id/PIXHAWK_ID \
    --lr24-device /dev/serial/by-id/LR24_ID \
    [options]

Required:
  --workspace PATH          Colcon workspace containing install/setup.bash
  --pixhawk-device PATH     Stable Pixhawk UART adapter /dev/serial/by-id path
  --lr24-device PATH        Stable airborne LR24 /dev/serial/by-id path

Options:
  --user USER               Non-root service user (default: the sudo caller)
  --ros-distro NAME         ROS distribution under /opt/ros (default: jazzy)
  --domain-id ID            ROS/PX4 DDS domain ID (default: 0)
  --pixhawk-baud BAUD       Pixhawk uXRCE-DDS baud (default: 921600)
  --lr24-baud BAUD          LR24 serial baud (default: 115200)
  --dds-agent PATH          MicroXRCEAgent executable (default: command -v)
  --device-wait SECONDS     0 waits forever; otherwise retry after timeout
  --no-start                Install/enable, stop it now, start at next boot
  -h, --help                Show this help
EOF
}

die()
{
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

warn()
{
    printf 'WARNING: %s\n' "$*" >&2
}

need_value()
{
    local option="$1"
    local value="${2:-}"
    [[ -n "$value" ]] || die "${option} requires a value"
}

while (( $# > 0 )); do
    case "$1" in
        --user)
            need_value "$1" "${2:-}"
            SERVICE_USER="$2"
            shift 2
            ;;
        --workspace)
            need_value "$1" "${2:-}"
            WORKSPACE="$2"
            shift 2
            ;;
        --pixhawk-device)
            need_value "$1" "${2:-}"
            PIXHAWK_DEVICE="$2"
            shift 2
            ;;
        --lr24-device)
            need_value "$1" "${2:-}"
            LR24_DEVICE="$2"
            shift 2
            ;;
        --ros-distro)
            need_value "$1" "${2:-}"
            ROS_DISTRO="$2"
            shift 2
            ;;
        --domain-id)
            need_value "$1" "${2:-}"
            ROS_DOMAIN_ID="$2"
            shift 2
            ;;
        --pixhawk-baud)
            need_value "$1" "${2:-}"
            PIXHAWK_BAUD="$2"
            shift 2
            ;;
        --lr24-baud)
            need_value "$1" "${2:-}"
            LR24_BAUD="$2"
            shift 2
            ;;
        --dds-agent)
            need_value "$1" "${2:-}"
            DDS_AGENT="$2"
            shift 2
            ;;
        --device-wait)
            need_value "$1" "${2:-}"
            DEVICE_WAIT_TIMEOUT_SECONDS="$2"
            shift 2
            ;;
        --no-start)
            START_NOW=0
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "Unknown option: $1 (use --help)"
            ;;
    esac
done

(( EUID == 0 )) || die "Run this installer with sudo"
[[ -n "$SERVICE_USER" ]] || die "Cannot determine the service user; pass --user"
[[ "$SERVICE_USER" != "root" ]] || die "The services must not run as root"
[[ "$SERVICE_USER" =~ ^[a-z_][a-z0-9_-]*\$?$ ]] ||
    die "Unsupported service user name: ${SERVICE_USER}"
getent passwd "$SERVICE_USER" >/dev/null || die "User does not exist: ${SERVICE_USER}"
getent group dialout >/dev/null || die "The dialout group does not exist"

[[ -n "$WORKSPACE" ]] || die "--workspace is required"
[[ "$WORKSPACE" == /* ]] || die "--workspace must be an absolute path"
[[ -n "$PIXHAWK_DEVICE" ]] || die "--pixhawk-device is required"
[[ -n "$LR24_DEVICE" ]] || die "--lr24-device is required"
[[ "$PIXHAWK_DEVICE" == /* ]] || die "--pixhawk-device must be an absolute path"
[[ "$LR24_DEVICE" == /* ]] || die "--lr24-device must be an absolute path"
[[ "$PIXHAWK_DEVICE" != "$LR24_DEVICE" ]] ||
    die "Pixhawk and LR24 must use different serial devices"

for value_name in ROS_DOMAIN_ID PIXHAWK_BAUD LR24_BAUD DEVICE_WAIT_TIMEOUT_SECONDS; do
    value="${!value_name}"
    [[ "$value" =~ ^(0|[1-9][0-9]*)$ ]] ||
        die "${value_name} must be an unsigned integer, got '${value}'"
done
(( ROS_DOMAIN_ID <= 232 )) || die "--domain-id must be between 0 and 232"
(( PIXHAWK_BAUD > 0 )) || die "--pixhawk-baud must be greater than zero"
case "$LR24_BAUD" in
    9600|19200|38400|57600|115200|230400)
        ;;
    *)
        die "Unsupported --lr24-baud: ${LR24_BAUD}"
        ;;
esac

[[ "$ROS_DISTRO" =~ ^[a-z][a-z0-9_]*$ ]] ||
    die "Invalid ROS distribution name: ${ROS_DISTRO}"
ROS_SETUP="/opt/ros/${ROS_DISTRO}/setup.bash"
[[ -r "$ROS_SETUP" ]] || die "ROS setup file not found: ${ROS_SETUP}"
[[ -r "${WORKSPACE}/install/setup.bash" ]] ||
    die "Workspace is not built or unreadable: ${WORKSPACE}/install/setup.bash"
WORKSPACE="$(realpath -e -- "$WORKSPACE")"

user_record="$(getent passwd "$SERVICE_USER")"
IFS=':' read -r _ _ _ _ _ USER_HOME _ <<<"$user_record"
[[ -n "$USER_HOME" && "$USER_HOME" == /* ]] ||
    die "Cannot determine an absolute home directory for ${SERVICE_USER}"

runuser -u "$SERVICE_USER" -- test -r "$ROS_SETUP" ||
    die "${SERVICE_USER} cannot read ${ROS_SETUP}"
runuser -u "$SERVICE_USER" -- test -r "${WORKSPACE}/install/setup.bash" ||
    die "${SERVICE_USER} cannot read ${WORKSPACE}/install/setup.bash"

if [[ -z "$DDS_AGENT" ]]; then
    DDS_AGENT="$(command -v MicroXRCEAgent || true)"
fi
[[ -n "$DDS_AGENT" ]] ||
    die "MicroXRCEAgent was not found; install v2.4.3 or pass --dds-agent"
if [[ "$DDS_AGENT" != /* ]]; then
    DDS_AGENT="$(command -v "$DDS_AGENT" || true)"
fi
[[ -n "$DDS_AGENT" && -x "$DDS_AGENT" ]] ||
    die "DDS Agent is not executable: ${DDS_AGENT:-<not found>}"
DDS_AGENT="$(realpath -e -- "$DDS_AGENT")"
runuser -u "$SERVICE_USER" -- test -x "$DDS_AGENT" ||
    die "${SERVICE_USER} cannot execute ${DDS_AGENT}"
DDS_VERSION_OUTPUT="$(
    runuser -u "$SERVICE_USER" -- "$DDS_AGENT" --version 2>&1
)" || die "DDS Agent failed its --version check as ${SERVICE_USER}"
if [[ "$DDS_VERSION_OUTPUT" != *"2.4.3"* ]]; then
    warn "Expected Micro XRCE-DDS Agent v2.4.3; reported: ${DDS_VERSION_OUTPUT}"
fi

runuser -u "$SERVICE_USER" -- mkdir -p "${USER_HOME}/.ros/log" ||
    die "${SERVICE_USER} cannot create ${USER_HOME}/.ros/log"
runuser -u "$SERVICE_USER" -- test -w "${USER_HOME}/.ros/log" ||
    die "${USER_HOME}/.ros/log is not writable by ${SERVICE_USER}"

package_prefix="$(
    runuser -u "$SERVICE_USER" -- env \
        "HOME=${USER_HOME}" \
        "USER=${SERVICE_USER}" \
        "LOGNAME=${SERVICE_USER}" \
        "NYCU_ROS_SETUP=${ROS_SETUP}" \
        "NYCU_WORKSPACE_SETUP=${WORKSPACE}/install/setup.bash" \
        /bin/bash -c '
            set -e
            source "$NYCU_ROS_SETUP"
            source "$NYCU_WORKSPACE_SETUP"
            ros2 pkg prefix my_offboard_cpp
        '
)" || die "my_offboard_cpp is not available in the selected workspace"
package_prefix="$(realpath -e -- "$package_prefix")"
workspace_install="$(realpath -e -- "${WORKSPACE}/install")"
[[ "$package_prefix" == "$workspace_install" ||
    "$package_prefix" == "${workspace_install}/"* ]] ||
    die "my_offboard_cpp resolves outside the selected workspace: ${package_prefix}"

installed_launch="${package_prefix}/share/my_offboard_cpp/launch/serial_gps_goto.launch.py"
[[ -r "$installed_launch" ]] ||
    die "Installed launch file is missing: ${installed_launch}"
cmp -s -- "${PROJECT_DIR}/launch/serial_gps_goto.launch.py" "$installed_launch" ||
    die "Installed launch file is stale; rebuild my_offboard_cpp before installing"

installed_lr24_node="${package_prefix}/lib/my_offboard_cpp/lr24_command_node"
[[ -x "$installed_lr24_node" ]] ||
    die "Installed LR24 node is missing: ${installed_lr24_node}"
[[ ! "${PROJECT_DIR}/src/lr24_command_node.cpp" -nt "$installed_lr24_node" ]] ||
    die "Installed LR24 node is older than its source; rebuild my_offboard_cpp"
grep -aFq "disappeared or changed; reconnecting" "$installed_lr24_node" ||
    die "Installed LR24 node lacks serial reconnect support; rebuild my_offboard_cpp"

runuser -u "$SERVICE_USER" -- env \
    "HOME=${USER_HOME}" \
    "USER=${SERVICE_USER}" \
    "LOGNAME=${SERVICE_USER}" \
    "NYCU_ROS_SETUP=${ROS_SETUP}" \
    "NYCU_WORKSPACE_SETUP=${WORKSPACE}/install/setup.bash" \
    "ROS_DOMAIN_ID=${ROS_DOMAIN_ID}" \
    /usr/bin/timeout 30s /bin/bash -c '
        set -e
        source "$NYCU_ROS_SETUP"
        source "$NYCU_WORKSPACE_SETUP"
        ros2 launch my_offboard_cpp serial_gps_goto.launch.py --show-args >/dev/null
    ' || die "The installed serial_gps_goto launch file failed validation"

for device in "$PIXHAWK_DEVICE" "$LR24_DEVICE"; do
    if [[ "$device" != /dev/serial/by-id/* ]]; then
        warn "Use a stable /dev/serial/by-id path instead of: ${device}"
    fi
    if [[ ! -c "$device" ]]; then
        warn "Serial device is not connected now; the service will wait for it: ${device}"
    fi
done

if systemctl is-active --quiet ModemManager.service; then
    warn "ModemManager is active and may probe the two serial devices."
    warn "Configure per-device ID_MM_DEVICE_IGNORE=1 rules if serial open errors occur."
fi

if [[ -c "$PIXHAWK_DEVICE" && -c "$LR24_DEVICE" ]]; then
    pixhawk_real="$(readlink -f -- "$PIXHAWK_DEVICE")"
    lr24_real="$(readlink -f -- "$LR24_DEVICE")"
    [[ "$pixhawk_real" != "$lr24_real" ]] ||
        die "Pixhawk and LR24 paths resolve to the same device: ${pixhawk_real}"
fi

for shell_file in \
    "${SCRIPT_DIR}/install.sh" \
    "${SCRIPT_DIR}/runtime/common.sh" \
    "${SCRIPT_DIR}/runtime/start-dds.sh" \
    "${SCRIPT_DIR}/runtime/start-topics.sh"
do
    /bin/bash -n "$shell_file" || die "Bash syntax check failed: ${shell_file}"
done

usermod -a -G dialout "$SERVICE_USER"

install -d -m 0755 "$CONFIG_DIR" "$RUNTIME_DEST"
install -m 0755 \
    "${SCRIPT_DIR}/runtime/common.sh" \
    "${SCRIPT_DIR}/runtime/start-dds.sh" \
    "${SCRIPT_DIR}/runtime/start-topics.sh" \
    "$RUNTIME_DEST/"
install -m 0644 \
    "${SCRIPT_DIR}/systemd/nycu-uav-dds@.service" \
    "${SCRIPT_DIR}/systemd/nycu-uav-topics@.service" \
    "${SCRIPT_DIR}/systemd/nycu-uav-offboard@.target" \
    "$SYSTEMD_DEST/"

desired_target="nycu-uav-offboard@${SERVICE_USER}.target"
/usr/bin/systemd-analyze verify \
    "nycu-uav-dds@${SERVICE_USER}.service" \
    "nycu-uav-topics@${SERVICE_USER}.service" \
    "$desired_target"

shopt -s nullglob
for enabled_path in \
    "${SYSTEMD_DEST}/multi-user.target.wants"/nycu-uav-offboard@*.target
do
    enabled_unit="$(basename -- "$enabled_path")"
    if [[ "$enabled_unit" != "$desired_target" ]]; then
        systemctl disable --now "$enabled_unit"
        printf 'Disabled previous NYCU UAV instance: %s\n' "$enabled_unit"
    fi
done
shopt -u nullglob

if [[ -f "$CONFIG_FILE" ]]; then
    backup="${CONFIG_FILE}.bak.$(date +%Y%m%d%H%M%S).$$"
    cp -a -- "$CONFIG_FILE" "$backup"
    printf 'Previous configuration backed up to %s\n' "$backup"
fi

temp_config="$(mktemp)"
cleanup()
{
    rm -f -- "$temp_config"
}
trap cleanup EXIT

{
    printf '# Generated by %q/deploy/rpi/install.sh\n' "$PROJECT_DIR"
    printf '# Edit with sudoedit, then restart nycu-uav-offboard@%s.target.\n' "$SERVICE_USER"
    printf 'NYCU_SERVICE_USER=%q\n' "$SERVICE_USER"
    printf 'NYCU_USER_HOME=%q\n' "$USER_HOME"
    printf 'NYCU_ROS_WS=%q\n' "$WORKSPACE"
    printf 'ROS_SETUP=%q\n' "$ROS_SETUP"
    printf 'ROS_DOMAIN_ID=%q\n' "$ROS_DOMAIN_ID"
    printf 'PIXHAWK_SERIAL=%q\n' "$PIXHAWK_DEVICE"
    printf 'PIXHAWK_BAUD=%q\n' "$PIXHAWK_BAUD"
    printf 'LR24_SERIAL=%q\n' "$LR24_DEVICE"
    printf 'LR24_BAUD=%q\n' "$LR24_BAUD"
    printf 'DDS_AGENT_BINARY=%q\n' "$DDS_AGENT"
    printf 'DEVICE_WAIT_TIMEOUT_SECONDS=%q\n' "$DEVICE_WAIT_TIMEOUT_SECONDS"
} >"$temp_config"
install -m 0644 "$temp_config" "$CONFIG_FILE"

systemctl daemon-reload
systemctl enable "$desired_target"

if (( START_NOW == 1 )); then
    systemctl restart "$desired_target"
else
    systemctl stop "$desired_target"
fi

cat <<EOF

NYCU UAV startup is installed for user ${SERVICE_USER}.
Configuration: ${CONFIG_FILE}
Enabled target: nycu-uav-offboard@${SERVICE_USER}.target

Check it with:
  systemctl --no-pager --full status nycu-uav-dds@${SERVICE_USER}.service
  systemctl --no-pager --full status nycu-uav-topics@${SERVICE_USER}.service
  journalctl -u nycu-uav-dds@${SERVICE_USER}.service -u nycu-uav-topics@${SERVICE_USER}.service -b
EOF

if (( START_NOW == 0 )); then
    printf '\nServices are enabled and will start at the next boot (--no-start was used).\n'
fi
