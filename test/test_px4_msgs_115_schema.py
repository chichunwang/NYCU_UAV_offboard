import os
import re
import unittest
from pathlib import Path


DECLARATION = re.compile(
    r"^(?P<type>[A-Za-z][A-Za-z0-9_/]*(?:\[[0-9]*\])?)\s+"
    r"(?P<name>[A-Za-z][A-Za-z0-9_]*)\s*(?:=\s*(?P<value>.+))?$"
)


REQUIRED_FIELDS = {
    "FailsafeFlags": {
        "angular_velocity_invalid",
        "attitude_invalid",
        "local_altitude_invalid",
        "local_position_invalid",
        "local_velocity_invalid",
        "global_position_invalid",
        "home_position_invalid",
        "manual_control_signal_lost",
        "battery_warning",
        "battery_low_remaining_time",
        "battery_unhealthy",
        "geofence_breached",
        "mission_failure",
        "vtol_fixed_wing_system_failure",
        "wind_limit_exceeded",
        "flight_time_limit_exceeded",
        "local_position_accuracy_low",
        "fd_critical_failure",
        "fd_esc_arming_failure",
        "fd_imbalanced_prop",
        "fd_motor_failure",
    },
    "HomePosition": {"lat", "lon", "alt", "valid_alt", "valid_hpos"},
    "OffboardControlMode": {
        "timestamp",
        "position",
        "velocity",
        "acceleration",
        "attitude",
        "body_rate",
        "thrust_and_torque",
        "direct_actuator",
    },
    "PositionSetpoint": {"valid", "type", "lat", "lon", "alt"},
    "PositionSetpointTriplet": {"current"},
    "SensorGps": {
        "fix_type",
        "eph",
        "epv",
        "jamming_state",
        "spoofing_state",
        "satellites_used",
    },
    "TrajectorySetpoint": {
        "timestamp",
        "position",
        "velocity",
        "acceleration",
        "yaw",
    },
    "VehicleCommand": {
        "timestamp",
        "param1",
        "param2",
        "param3",
        "param4",
        "param5",
        "param6",
        "param7",
        "command",
        "target_system",
        "target_component",
        "source_system",
        "source_component",
        "confirmation",
        "from_external",
    },
    "VehicleCommandAck": {
        "command",
        "result",
        "target_system",
        "target_component",
    },
    "VehicleGlobalPosition": {
        "lat",
        "lon",
        "alt",
        "eph",
        "epv",
        "dead_reckoning",
    },
    "VehicleLandDetected": {"freefall", "ground_contact", "maybe_landed", "landed"},
    "VehicleStatus": {
        "arming_state",
        "nav_state",
        "failure_detector_status",
        "vehicle_type",
        "failsafe",
        "failsafe_and_user_took_over",
        "failsafe_defer_state",
        "in_transition_mode",
        "system_id",
        "component_id",
    },
}


REQUIRED_CONSTANTS = {
    "PositionSetpoint": {"SETPOINT_TYPE_LOITER"},
    "SensorGps": {
        "FIX_TYPE_3D",
        "FIX_TYPE_RTK_FIXED",
        "FIX_TYPE_EXTRAPOLATED",
        "JAMMING_STATE_WARNING",
        "JAMMING_STATE_CRITICAL",
        "SPOOFING_STATE_INDICATED",
        "SPOOFING_STATE_MULTIPLE",
    },
    "VehicleCommand": {
        "VEHICLE_CMD_COMPONENT_ARM_DISARM",
        "VEHICLE_CMD_DO_REPOSITION",
        "VEHICLE_CMD_DO_SET_MODE",
        "VEHICLE_CMD_NAV_LAND",
        "VEHICLE_CMD_NAV_RETURN_TO_LAUNCH",
    },
    "VehicleCommandAck": {
        "VEHICLE_CMD_RESULT_ACCEPTED",
        "VEHICLE_CMD_RESULT_TEMPORARILY_REJECTED",
        "VEHICLE_CMD_RESULT_DENIED",
        "VEHICLE_CMD_RESULT_UNSUPPORTED",
        "VEHICLE_CMD_RESULT_FAILED",
        "VEHICLE_CMD_RESULT_IN_PROGRESS",
        "VEHICLE_CMD_RESULT_CANCELLED",
    },
    "VehicleStatus": {
        "ARMING_STATE_ARMED",
        "FAILURE_NONE",
        "FAILSAFE_DEFER_STATE_WOULD_FAILSAFE",
        "NAVIGATION_STATE_AUTO_LAND",
        "NAVIGATION_STATE_AUTO_LOITER",
        "NAVIGATION_STATE_AUTO_RTL",
        "NAVIGATION_STATE_AUTO_TAKEOFF",
        "NAVIGATION_STATE_DESCEND",
        "NAVIGATION_STATE_TERMINATION",
        "VEHICLE_TYPE_FIXED_WING",
    },
}


FORBIDDEN_LATER_SYMBOLS = {
    "FailsafeFlags": {
        "global_position_invalid_relaxed",
        "navigator_failure",
        "position_accuracy_low",
    },
    "SensorGps": {
        "authentication_state",
        "AUTHENTICATION_STATE_ERROR",
        "system_error",
        "SYSTEM_ERROR_OK",
        "JAMMING_STATE_DETECTED",
        "SPOOFING_STATE_DETECTED",
    },
    "VehicleGlobalPosition": {"lat_lon_valid", "alt_valid"},
}


def find_px4_msgs_directory():
    configured = os.environ.get("PX4_MSGS_DIR")
    candidates = []
    if configured:
        candidates.append(Path(configured))

    source_root = Path(__file__).resolve().parents[2]
    candidates.append(source_root / "px4_msgs" / "msg")

    for root in (Path.cwd(), *Path.cwd().parents):
        candidates.append(root / "src" / "px4_msgs" / "msg")
        candidates.append(root / "px4_msgs" / "msg")

    for variable in ("AMENT_PREFIX_PATH", "CMAKE_PREFIX_PATH", "COLCON_PREFIX_PATH"):
        for prefix in os.environ.get(variable, "").split(os.pathsep):
            if prefix:
                candidates.append(Path(prefix) / "share" / "px4_msgs" / "msg")

    for candidate in candidates:
        msg_directory = candidate / "msg" if candidate.name != "msg" else candidate
        if (msg_directory / "SensorGps.msg").is_file():
            return msg_directory
    return None


def parse_message(path):
    fields = set()
    constants = set()
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        declaration = raw_line.split("#", 1)[0].strip()
        if not declaration:
            continue
        match = DECLARATION.match(declaration)
        if not match:
            raise AssertionError(f"Cannot parse declaration in {path}: {raw_line!r}")
        destination = constants if match.group("value") is not None else fields
        destination.add(match.group("name"))
    return fields, constants


class Px4Msgs115SchemaTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.msg_directory = find_px4_msgs_directory()
        if cls.msg_directory is None:
            raise AssertionError(
                "px4_msgs/msg not found; set PX4_MSGS_DIR or place px4_msgs beside this package"
            )

    def test_required_release_115_contract(self):
        for message_name, required_fields in REQUIRED_FIELDS.items():
            fields, constants = parse_message(
                self.msg_directory / f"{message_name}.msg"
            )
            self.assertFalse(
                required_fields - fields,
                f"{message_name} missing fields: {sorted(required_fields - fields)}",
            )

            required_constants = REQUIRED_CONSTANTS.get(message_name, set())
            self.assertFalse(
                required_constants - constants,
                f"{message_name} missing constants: "
                f"{sorted(required_constants - constants)}",
            )

    def test_later_release_symbols_are_absent(self):
        for message_name, forbidden_symbols in FORBIDDEN_LATER_SYMBOLS.items():
            fields, constants = parse_message(
                self.msg_directory / f"{message_name}.msg"
            )
            present = forbidden_symbols & (fields | constants)
            self.assertFalse(
                present,
                f"{message_name} contains later-release symbols: {sorted(present)}; "
                "checkout px4_msgs release/1.15",
            )


if __name__ == "__main__":
    unittest.main()
