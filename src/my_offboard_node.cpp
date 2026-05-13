#include <rclcpp/rclcpp.hpp>

#include <px4_msgs/msg/offboard_control_mode.hpp>
#include <px4_msgs/msg/trajectory_setpoint.hpp>
#include <px4_msgs/msg/vehicle_command.hpp>

#include <chrono>
#include <cmath>
#include <array>

using namespace std::chrono_literals;

class MyOffboardNode : public rclcpp::Node
{
public:
    MyOffboardNode() : Node("my_offboard_node")
    {
        offboard_control_mode_pub_ =
            this->create_publisher<px4_msgs::msg::OffboardControlMode>(
                "/fmu/in/offboard_control_mode", 10);

        trajectory_setpoint_pub_ =
            this->create_publisher<px4_msgs::msg::TrajectorySetpoint>(
                "/fmu/in/trajectory_setpoint", 10);

        vehicle_command_pub_ =
            this->create_publisher<px4_msgs::msg::VehicleCommand>(
                "/fmu/in/vehicle_command", 10);

        timer_ = this->create_wall_timer(
            100ms,
            std::bind(&MyOffboardNode::timer_callback, this));

        RCLCPP_INFO(this->get_logger(), "My offboard node started.");
    }

private:
    rclcpp::Publisher<px4_msgs::msg::OffboardControlMode>::SharedPtr offboard_control_mode_pub_;
    rclcpp::Publisher<px4_msgs::msg::TrajectorySetpoint>::SharedPtr trajectory_setpoint_pub_;
    rclcpp::Publisher<px4_msgs::msg::VehicleCommand>::SharedPtr vehicle_command_pub_;
    rclcpp::TimerBase::SharedPtr timer_;

    int counter_ = 0;

    uint64_t timestamp_us()
    {
        return this->get_clock()->now().nanoseconds() / 1000;
    }

    void timer_callback()
    {
        publish_offboard_control_mode();

        std::array<float, 3> target_position = get_target_position();
        publish_trajectory_setpoint(target_position[0], target_position[1], target_position[2], 0.0f);

        /*
         * 前 10 次只送 setpoint。
         * 因為 timer 是 100 ms，所以 10 次約等於 1 秒。
         * PX4 需要先收到 Offboard 訊號，才允許切入 Offboard。
         */
        if (counter_ == 10) {
            set_offboard_mode();
            arm();
            RCLCPP_INFO(this->get_logger(), "Switch to Offboard mode and arm.");
        }

        if (counter_ < 100000) {
            counter_++;
        }
    }

    std::array<float, 3> get_target_position()
    {
        /*
         * NED 座標：
         * x: North
         * y: East
         * z: Down
         *
         * z = -5 代表高度 5 m。
         */

        if (counter_ < 100) {
            // 起飛並停在原點上方 5 m
            return {0.0f, 0.0f, -5.0f};
        } else if (counter_ < 200) {
            // 往 x 方向 5 m
            return {5.0f, 0.0f, -5.0f};
        } else if (counter_ < 300) {
            // 往 y 方向 5 m
            return {5.0f, 5.0f, -5.0f};
        } else if (counter_ < 400) {
            // 回到 x = 0
            return {0.0f, 5.0f, -5.0f};
        } else if (counter_ < 500) {
            // 回到原點上方
            return {0.0f, 0.0f, -5.0f};
        } else {
            // 最後懸停
            return {0.0f, 0.0f, -5.0f};
        }
    }

    void publish_offboard_control_mode()
    {
        px4_msgs::msg::OffboardControlMode msg{};

        msg.timestamp = timestamp_us();

        msg.position = true;
        msg.velocity = false;
        msg.acceleration = false;
        msg.attitude = false;
        msg.body_rate = false;
        msg.thrust_and_torque = false;
        msg.direct_actuator = false;

        offboard_control_mode_pub_->publish(msg);
    }

    void publish_trajectory_setpoint(float x, float y, float z, float yaw)
    {
        px4_msgs::msg::TrajectorySetpoint msg{};

        msg.timestamp = timestamp_us();

        msg.position = {x, y, z};
        msg.velocity = {NAN, NAN, NAN};
        msg.acceleration = {NAN, NAN, NAN};
        msg.yaw = yaw;

        trajectory_setpoint_pub_->publish(msg);
    }

    void publish_vehicle_command(uint16_t command, float param1 = 0.0f, float param2 = 0.0f)
    {
        px4_msgs::msg::VehicleCommand msg{};

        msg.timestamp = timestamp_us();

        msg.param1 = param1;
        msg.param2 = param2;
        msg.command = command;

        msg.target_system = 1;
        msg.target_component = 1;
        msg.source_system = 1;
        msg.source_component = 1;

        msg.from_external = true;

        vehicle_command_pub_->publish(msg);
    }

    void arm()
    {
        publish_vehicle_command(
            px4_msgs::msg::VehicleCommand::VEHICLE_CMD_COMPONENT_ARM_DISARM,
            1.0f,
            0.0f);
    }

    void disarm()
    {
        publish_vehicle_command(
            px4_msgs::msg::VehicleCommand::VEHICLE_CMD_COMPONENT_ARM_DISARM,
            0.0f,
            0.0f);
    }

    void set_offboard_mode()
    {
        publish_vehicle_command(
            px4_msgs::msg::VehicleCommand::VEHICLE_CMD_DO_SET_MODE,
            1.0f,
            6.0f);
    }
};

int main(int argc, char * argv[])
{
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<MyOffboardNode>());
    rclcpp::shutdown();
    return 0;
}