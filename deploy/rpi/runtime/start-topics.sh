#!/usr/bin/env bash

set -Eeuo pipefail

readonly RUNTIME_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck source=common.sh
source "${RUNTIME_DIR}/common.sh"

load_config
wait_for_serial_device "LR24" "$LR24_SERIAL"

[[ -r "$ROS_SETUP" ]] || die "ROS setup file is not readable: ${ROS_SETUP}"
workspace_setup="${NYCU_ROS_WS}/install/setup.bash"
[[ -r "$workspace_setup" ]] ||
    die "Workspace setup file is not readable: ${workspace_setup}; build the workspace first"

mkdir -p "${NYCU_USER_HOME}/.ros/log"
export ROS_LOG_DIR="${NYCU_USER_HOME}/.ros/log"
export RCUTILS_COLORIZED_OUTPUT=0
export PYTHONUNBUFFERED=1

# ROS setup scripts are not guaranteed to be nounset-clean.
set +u
# shellcheck disable=SC1090
source "$ROS_SETUP"
# shellcheck disable=SC1090
source "$workspace_setup"
set -u

command -v ros2 >/dev/null 2>&1 || die "ros2 was not found after sourcing ROS"

log "Starting GPS GOTO ROS nodes; PX4 /fmu topics will appear when DDS connects"
exec ros2 launch my_offboard_cpp serial_gps_goto.launch.py \
    lr24_port:="$LR24_SERIAL" \
    lr24_baud_rate:="$LR24_BAUD"

