#include <rclcpp/rclcpp.hpp>

#include <px4_msgs/msg/offboard_control_mode.hpp>
#include <px4_msgs/msg/trajectory_setpoint.hpp>
#include <px4_msgs/msg/vehicle_command.hpp>
#include <std_srvs/srv/trigger.hpp>

#include <chrono>
#include <cmath>
#include <array>
#include <functional>
#include <sstream>

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

        stream_active_ = this->declare_parameter<bool>("stream_on_start", true);

        start_offboard_srv_ = this->create_service<std_srvs::srv::Trigger>(
            "start_offboard",
            std::bind(
                &MyOffboardNode::start_offboard_callback,
                this,
                std::placeholders::_1,
                std::placeholders::_2));

        enable_stream_srv_ = this->create_service<std_srvs::srv::Trigger>(
            "enable_offboard_stream",
            std::bind(
                &MyOffboardNode::enable_stream_callback,
                this,
                std::placeholders::_1,
                std::placeholders::_2));

        start_mission_srv_ = this->create_service<std_srvs::srv::Trigger>(
            "start_mission",
            std::bind(
                &MyOffboardNode::start_mission_callback,
                this,
                std::placeholders::_1,
                std::placeholders::_2));

        stop_offboard_srv_ = this->create_service<std_srvs::srv::Trigger>(
            "stop_offboard",
            std::bind(
                &MyOffboardNode::stop_offboard_callback,
                this,
                std::placeholders::_1,
                std::placeholders::_2));

        land_srv_ = this->create_service<std_srvs::srv::Trigger>(
            "land",
            std::bind(
                &MyOffboardNode::land_callback,
                this,
                std::placeholders::_1,
                std::placeholders::_2));

        status_srv_ = this->create_service<std_srvs::srv::Trigger>(
            "offboard_status",
            std::bind(
                &MyOffboardNode::status_callback,
                this,
                std::placeholders::_1,
                std::placeholders::_2));

        RCLCPP_INFO(
            this->get_logger(),
            "My offboard node started. stream_active=%s. Call /start_offboard for serial-controlled mode switch, or use RC/ELRS after stream warmup.",
            stream_active_ ? "true" : "false");
    }

private:
    rclcpp::Publisher<px4_msgs::msg::OffboardControlMode>::SharedPtr offboard_control_mode_pub_;
    rclcpp::Publisher<px4_msgs::msg::TrajectorySetpoint>::SharedPtr trajectory_setpoint_pub_;
    rclcpp::Publisher<px4_msgs::msg::VehicleCommand>::SharedPtr vehicle_command_pub_;
    rclcpp::TimerBase::SharedPtr timer_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr start_offboard_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr enable_stream_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr start_mission_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr stop_offboard_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr land_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr status_srv_;

    int counter_ = 0;
    int stream_counter_ = 0;
    bool stream_active_ = true;
    bool command_mode_after_warmup_ = false;
    bool mission_active_ = false;
    bool offboard_command_sent_ = false;

    uint64_t timestamp_us()
    {
        return this->get_clock()->now().nanoseconds() / 1000;
    }

    void timer_callback()
    {
        if (!stream_active_) {
            return;
        }

        publish_offboard_control_mode();

        std::array<float, 3> target_position = get_target_position();
        publish_trajectory_setpoint(target_position[0], target_position[1], target_position[2], 0.0f);

        /*
         * 前 10 次只送 setpoint。
         * 因為 timer 是 100 ms，所以 10 次約等於 1 秒。
         * PX4 需要先收到 Offboard 訊號，才允許切入 Offboard。
         */
        if (command_mode_after_warmup_ && stream_counter_ >= 10 && !offboard_command_sent_) {
            set_offboard_mode();
            arm();
            offboard_command_sent_ = true;
            RCLCPP_INFO(this->get_logger(), "Switch to Offboard mode and arm.");
        }

        if (mission_active_ && counter_ < 100000) {
            counter_++;
        }

        if (stream_counter_ < 100000) {
            stream_counter_++;
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

        if (!mission_active_) {
            // RC/ELRS 切入 Offboard 前只送固定 setpoint，避免節點啟動後任務路徑自行前進。
            return {0.0f, 0.0f, -5.0f};
        } else if (counter_ < 100) {
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

    void land()
    {
        publish_vehicle_command(
            px4_msgs::msg::VehicleCommand::VEHICLE_CMD_NAV_LAND,
            0.0f,
            0.0f);
    }

    void start_offboard_callback(
        const std::shared_ptr<std_srvs::srv::Trigger::Request> request,
        std::shared_ptr<std_srvs::srv::Trigger::Response> response)
    {
        (void)request;

        counter_ = 0;
        stream_counter_ = 0;
        stream_active_ = true;
        command_mode_after_warmup_ = true;
        mission_active_ = true;
        offboard_command_sent_ = false;

        response->success = true;
        response->message = "Offboard stream and mission started. PX4 mode switch and arm will be sent after setpoint warmup.";
        RCLCPP_WARN(this->get_logger(), "Serial-controlled Offboard start requested.");
    }

    void enable_stream_callback(
        const std::shared_ptr<std_srvs::srv::Trigger::Request> request,
        std::shared_ptr<std_srvs::srv::Trigger::Response> response)
    {
        (void)request;

        counter_ = 0;
        stream_counter_ = 0;
        stream_active_ = true;
        command_mode_after_warmup_ = false;
        mission_active_ = false;
        offboard_command_sent_ = false;

        response->success = true;
        response->message = "Offboard stream enabled for RC/ELRS mode switching. PX4 mode and arm commands will not be sent by Orin.";
        RCLCPP_WARN(this->get_logger(), "Offboard stream enabled for RC/ELRS flow.");
    }

    void start_mission_callback(
        const std::shared_ptr<std_srvs::srv::Trigger::Request> request,
        std::shared_ptr<std_srvs::srv::Trigger::Response> response)
    {
        (void)request;

        if (!stream_active_) {
            response->success = false;
            response->message = "Cannot start mission because offboard stream is disabled.";
            return;
        }

        counter_ = 0;
        mission_active_ = true;
        command_mode_after_warmup_ = false;

        response->success = true;
        response->message = "Mission trajectory started without sending PX4 mode or arm commands.";
        RCLCPP_WARN(this->get_logger(), "Mission trajectory start requested.");
    }

    void stop_offboard_callback(
        const std::shared_ptr<std_srvs::srv::Trigger::Request> request,
        std::shared_ptr<std_srvs::srv::Trigger::Response> response)
    {
        (void)request;

        stream_active_ = false;
        command_mode_after_warmup_ = false;
        mission_active_ = false;
        offboard_command_sent_ = false;
        counter_ = 0;
        stream_counter_ = 0;

        response->success = true;
        response->message = "Offboard setpoint publishing stopped. Switch PX4 to a safe RC/manual mode before using this in flight.";
        RCLCPP_WARN(this->get_logger(), "Offboard stop requested.");
    }

    void land_callback(
        const std::shared_ptr<std_srvs::srv::Trigger::Request> request,
        std::shared_ptr<std_srvs::srv::Trigger::Response> response)
    {
        (void)request;

        land();
        stream_active_ = false;
        command_mode_after_warmup_ = false;
        mission_active_ = false;
        offboard_command_sent_ = false;
        counter_ = 0;
        stream_counter_ = 0;

        response->success = true;
        response->message = "Land command sent and offboard setpoint publishing stopped.";
        RCLCPP_WARN(this->get_logger(), "Land requested.");
    }

    void status_callback(
        const std::shared_ptr<std_srvs::srv::Trigger::Request> request,
        std::shared_ptr<std_srvs::srv::Trigger::Response> response)
    {
        (void)request;

        std::ostringstream status;
        status << "stream_active=" << (stream_active_ ? "true" : "false")
               << ",mission_active=" << (mission_active_ ? "true" : "false")
               << ",command_mode_after_warmup=" << (command_mode_after_warmup_ ? "true" : "false")
               << ",offboard_command_sent=" << (offboard_command_sent_ ? "true" : "false")
               << ",stream_counter=" << stream_counter_
               << ",mission_counter=" << counter_;

        response->success = true;
        response->message = status.str();
    }
};

int main(int argc, char * argv[])
{
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<MyOffboardNode>());
    rclcpp::shutdown();
    return 0;
}
