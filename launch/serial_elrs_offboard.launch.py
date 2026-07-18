from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterValue


def generate_launch_description():
    port = LaunchConfiguration("port")
    baud_rate = LaunchConfiguration("baud_rate")
    stream_on_start = LaunchConfiguration("stream_on_start")

    return LaunchDescription([
        DeclareLaunchArgument("port", default_value="/dev/ttyUSB0"),
        DeclareLaunchArgument("baud_rate", default_value="115200"),
        DeclareLaunchArgument("stream_on_start", default_value="true"),

        Node(
            package="my_offboard_cpp",
            executable="my_offboard_node",
            name="my_offboard_node",
            output="screen",
            parameters=[{
                "stream_on_start": ParameterValue(stream_on_start, value_type=bool),
            }],
        ),

        Node(
            package="my_offboard_cpp",
            executable="lr24_command_node",
            name="lr24_command_node",
            output="screen",
            parameters=[{
                "port": port,
                "baud_rate": ParameterValue(baud_rate, value_type=int),
            }],
        ),
    ])
