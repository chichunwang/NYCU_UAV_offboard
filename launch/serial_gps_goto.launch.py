from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterValue


def generate_launch_description():
    lr24_port = LaunchConfiguration("lr24_port")
    lr24_baud_rate = LaunchConfiguration("lr24_baud_rate")
    lr24_service_response_timeout_s = LaunchConfiguration(
        "lr24_service_response_timeout_s"
    )

    float_parameters = {
        "telemetry_timeout_s": "1.0",
        "gps_max_horizontal_accuracy_m": "5.0",
        "gps_max_vertical_accuracy_m": "8.0",
        "max_target_distance_m": "2000.0",
        "min_relative_altitude_m": "30.0",
        "max_relative_altitude_m": "120.0",
        "ack_timeout_s": "2.0",
        "confirmation_timeout_s": "3.0",
        "setpoint_horizontal_tolerance_m": "5.0",
        "setpoint_altitude_tolerance_m": "2.0",
        "arrival_horizontal_threshold_m": "100.0",
        "arrival_vertical_threshold_m": "15.0",
        "arrival_hold_time_s": "2.0",
        "monitor_rate_hz": "5.0",
    }
    integer_parameters = {
        "gps_min_fix_type": "3",
        "gps_min_satellites": "8",
        "source_system_id": "1",
        "source_component_id": "191",
    }

    arguments = [
        DeclareLaunchArgument(
            "lr24_port",
            description=(
                "Airborne LR24 serial device. Prefer a stable "
                "/dev/serial/by-id/... path; this is not the Pixhawk TELEM2 device."
            ),
        ),
        DeclareLaunchArgument(
            "lr24_baud_rate",
            default_value="115200",
            description="USB serial baud rate between the airborne computer and LR24-F.",
        ),
        DeclareLaunchArgument(
            "lr24_service_response_timeout_s",
            default_value="7.0",
            description=(
                "Maximum wait for an airborne ROS service response before its client "
                "request is removed and LR24 receives ERR."
            ),
        ),
    ]

    for name, default_value in float_parameters.items():
        arguments.append(
            DeclareLaunchArgument(name, default_value=default_value)
        )

    for name, default_value in integer_parameters.items():
        arguments.append(
            DeclareLaunchArgument(name, default_value=default_value)
        )

    global_goto_parameters = {
        name: ParameterValue(LaunchConfiguration(name), value_type=float)
        for name in float_parameters
    }
    global_goto_parameters.update({
        name: ParameterValue(LaunchConfiguration(name), value_type=int)
        for name in integer_parameters
    })

    return LaunchDescription(arguments + [
        Node(
            package="my_offboard_cpp",
            executable="global_goto_node",
            name="global_goto_node",
            output="screen",
            parameters=[global_goto_parameters],
        ),
        Node(
            package="my_offboard_cpp",
            executable="lr24_command_node",
            name="lr24_command_node",
            output="screen",
            parameters=[{
                "port": lr24_port,
                "baud_rate": ParameterValue(lr24_baud_rate, value_type=int),
                "service_response_timeout_s": ParameterValue(
                    lr24_service_response_timeout_s, value_type=float
                ),
                "goto_global_service": "/goto_global",
                "status_service": "/gps_goto_status",
                "return_to_launch_service": "/return_to_launch",
            }],
        ),
    ])
