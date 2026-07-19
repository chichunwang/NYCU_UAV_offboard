#include <rclcpp/executors/multi_threaded_executor.hpp>
#include <rclcpp/rclcpp.hpp>

#include <my_offboard_cpp/srv/goto_global.hpp>
#include <px4_msgs/msg/failsafe_flags.hpp>
#include <px4_msgs/msg/home_position.hpp>
#include <px4_msgs/msg/position_setpoint.hpp>
#include <px4_msgs/msg/position_setpoint_triplet.hpp>
#include <px4_msgs/msg/sensor_gps.hpp>
#include <px4_msgs/msg/vehicle_command.hpp>
#include <px4_msgs/msg/vehicle_command_ack.hpp>
#include <px4_msgs/msg/vehicle_global_position.hpp>
#include <px4_msgs/msg/vehicle_land_detected.hpp>
#include <px4_msgs/msg/vehicle_status.hpp>
#include <std_srvs/srv/trigger.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace std::chrono_literals;

class GlobalGotoNode : public rclcpp::Node
{
public:
    using GotoGlobal = my_offboard_cpp::srv::GotoGlobal;
    using Trigger = std_srvs::srv::Trigger;
    using FailsafeFlags = px4_msgs::msg::FailsafeFlags;
    using HomePosition = px4_msgs::msg::HomePosition;
    using PositionSetpoint = px4_msgs::msg::PositionSetpoint;
    using PositionSetpointTriplet = px4_msgs::msg::PositionSetpointTriplet;
    using SensorGps = px4_msgs::msg::SensorGps;
    using VehicleCommand = px4_msgs::msg::VehicleCommand;
    using VehicleCommandAck = px4_msgs::msg::VehicleCommandAck;
    using VehicleGlobalPosition = px4_msgs::msg::VehicleGlobalPosition;
    using VehicleLandDetected = px4_msgs::msg::VehicleLandDetected;
    using VehicleStatus = px4_msgs::msg::VehicleStatus;
    using SteadyClock = std::chrono::steady_clock;
    using SteadyTime = SteadyClock::time_point;

    GlobalGotoNode()
    : Node("global_goto_node")
    {
        telemetry_timeout_s_ = this->declare_parameter<double>("telemetry_timeout_s", 1.0);
        land_detected_timeout_s_ =
            this->declare_parameter<double>("land_detected_timeout_s", 2.5);
        gps_min_fix_type_ = this->declare_parameter<int>("gps_min_fix_type", 3);
        gps_min_satellites_ = this->declare_parameter<int>("gps_min_satellites", 8);
        gps_max_horizontal_accuracy_m_ =
            this->declare_parameter<double>("gps_max_horizontal_accuracy_m", 5.0);
        gps_max_vertical_accuracy_m_ =
            this->declare_parameter<double>("gps_max_vertical_accuracy_m", 8.0);
        max_target_distance_m_ =
            this->declare_parameter<double>("max_target_distance_m", 2000.0);
        min_relative_altitude_m_ =
            this->declare_parameter<double>("min_relative_altitude_m", 30.0);
        max_relative_altitude_m_ =
            this->declare_parameter<double>("max_relative_altitude_m", 120.0);
        ack_timeout_s_ = this->declare_parameter<double>("ack_timeout_s", 2.0);
        confirmation_timeout_s_ =
            this->declare_parameter<double>("confirmation_timeout_s", 3.0);
        setpoint_horizontal_tolerance_m_ =
            this->declare_parameter<double>("setpoint_horizontal_tolerance_m", 5.0);
        setpoint_altitude_tolerance_m_ =
            this->declare_parameter<double>("setpoint_altitude_tolerance_m", 2.0);
        arrival_horizontal_threshold_m_ =
            this->declare_parameter<double>("arrival_horizontal_threshold_m", 100.0);
        arrival_vertical_threshold_m_ =
            this->declare_parameter<double>("arrival_vertical_threshold_m", 15.0);
        arrival_hold_time_s_ = this->declare_parameter<double>("arrival_hold_time_s", 2.0);
        monitor_rate_hz_ = this->declare_parameter<double>("monitor_rate_hz", 5.0);
        source_system_id_ = this->declare_parameter<int>("source_system_id", 1);
        source_component_id_ = this->declare_parameter<int>("source_component_id", 191);

        validate_parameters();

        callback_group_ =
            this->create_callback_group(rclcpp::CallbackGroupType::Reentrant);

        vehicle_command_pub_ = this->create_publisher<VehicleCommand>(
            "/fmu/in/vehicle_command", rclcpp::QoS(10));

        rclcpp::SubscriptionOptions subscription_options;
        subscription_options.callback_group = callback_group_;
        const auto sensor_qos = rclcpp::SensorDataQoS();
        const auto retained_sensor_qos =
            rclcpp::QoS(rclcpp::KeepLast(1)).best_effort().transient_local();

        global_position_sub_ = this->create_subscription<VehicleGlobalPosition>(
            "/fmu/out/vehicle_global_position",
            sensor_qos,
            [this](const VehicleGlobalPosition::SharedPtr msg) {
                store_telemetry(global_position_, *msg);
            },
            subscription_options);

        gps_sub_ = this->create_subscription<SensorGps>(
            "/fmu/out/vehicle_gps_position",
            sensor_qos,
            [this](const SensorGps::SharedPtr msg) {
                store_telemetry(gps_, *msg);
            },
            subscription_options);

        home_position_sub_ = this->create_subscription<HomePosition>(
            "/fmu/out/home_position",
            retained_sensor_qos,
            [this](const HomePosition::SharedPtr msg) {
                store_telemetry(home_position_, *msg);
            },
            subscription_options);

        vehicle_status_sub_ = this->create_subscription<VehicleStatus>(
            "/fmu/out/vehicle_status",
            sensor_qos,
            [this](const VehicleStatus::SharedPtr msg) {
                store_telemetry(vehicle_status_, *msg);
            },
            subscription_options);

        land_detected_sub_ = this->create_subscription<VehicleLandDetected>(
            "/fmu/out/vehicle_land_detected",
            sensor_qos,
            [this](const VehicleLandDetected::SharedPtr msg) {
                store_telemetry(land_detected_, *msg);
            },
            subscription_options);

        failsafe_flags_sub_ = this->create_subscription<FailsafeFlags>(
            "/fmu/out/failsafe_flags",
            sensor_qos,
            [this](const FailsafeFlags::SharedPtr msg) {
                store_telemetry(failsafe_flags_, *msg);
            },
            subscription_options);

        command_ack_sub_ = this->create_subscription<VehicleCommandAck>(
            "/fmu/out/vehicle_command_ack",
            sensor_qos,
            std::bind(&GlobalGotoNode::command_ack_callback, this, std::placeholders::_1),
            subscription_options);

        position_setpoint_triplet_sub_ = this->create_subscription<PositionSetpointTriplet>(
            "/fmu/out/position_setpoint_triplet",
            sensor_qos,
            [this](const PositionSetpointTriplet::SharedPtr msg) {
                store_telemetry(position_setpoint_triplet_, *msg);
            },
            subscription_options);

        goto_global_srv_ = this->create_service<GotoGlobal>(
            "/goto_global",
            std::bind(
                &GlobalGotoNode::goto_global_callback,
                this,
                std::placeholders::_1,
                std::placeholders::_2),
            rclcpp::ServicesQoS(),
            callback_group_);

        status_srv_ = this->create_service<Trigger>(
            "/gps_goto_status",
            std::bind(
                &GlobalGotoNode::status_callback,
                this,
                std::placeholders::_1,
                std::placeholders::_2),
            rclcpp::ServicesQoS(),
            callback_group_);

        rtl_srv_ = this->create_service<Trigger>(
            "/return_to_launch",
            std::bind(
                &GlobalGotoNode::rtl_callback,
                this,
                std::placeholders::_1,
                std::placeholders::_2),
            rclcpp::ServicesQoS(),
            callback_group_);

        const auto monitor_period = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>(1.0 / monitor_rate_hz_));
        monitor_timer_ = this->create_wall_timer(
            monitor_period,
            std::bind(&GlobalGotoNode::monitor_callback, this),
            callback_group_);

        RCLCPP_INFO(
            this->get_logger(),
            "Fixed-wing global goto node started. It never arms, takes off, lands, or streams Offboard setpoints.");
    }

private:
    template<typename MessageT>
    struct TimedTelemetry
    {
        MessageT message{};
        SteadyTime received_at{};
        uint64_t generation{0};
        bool received{false};
    };

    struct Target
    {
        double latitude_deg{0.0};
        double longitude_deg{0.0};
        double altitude_amsl_m{0.0};
        double altitude_relative_home_m{0.0};
        double initial_distance_m{0.0};
        double home_distance_m{0.0};
        uint8_t target_system{0};
        uint8_t target_component{0};
    };

    struct AckRecord
    {
        uint64_t generation{0};
        VehicleCommandAck message{};
    };

    struct AckWaitResult
    {
        bool received{false};
        uint8_t result{VehicleCommandAck::VEHICLE_CMD_RESULT_FAILED};
        bool preempted{false};
    };

    struct RtlRequestGuard
    {
        explicit RtlRequestGuard(std::atomic<uint32_t> & counter)
        : counter_(counter)
        {
            counter_.fetch_add(1U, std::memory_order_acq_rel);
        }

        ~RtlRequestGuard()
        {
            counter_.fetch_sub(1U, std::memory_order_acq_rel);
        }

        RtlRequestGuard(const RtlRequestGuard &) = delete;
        RtlRequestGuard & operator=(const RtlRequestGuard &) = delete;

    private:
        std::atomic<uint32_t> & counter_;
    };

    struct RtlCallbackGuard
    {
        explicit RtlCallbackGuard(std::atomic<bool> & active)
        : active_(active) {}

        ~RtlCallbackGuard()
        {
            active_.store(false, std::memory_order_release);
        }

        RtlCallbackGuard(const RtlCallbackGuard &) = delete;
        RtlCallbackGuard & operator=(const RtlCallbackGuard &) = delete;

    private:
        std::atomic<bool> & active_;
    };

    enum class GotoState
    {
        Idle,
        CommandPending,
        Enroute,
        Arrived,
        Aborted
    };

    double telemetry_timeout_s_{1.0};
    double land_detected_timeout_s_{2.5};
    int gps_min_fix_type_{3};
    int gps_min_satellites_{8};
    double gps_max_horizontal_accuracy_m_{5.0};
    double gps_max_vertical_accuracy_m_{8.0};
    double max_target_distance_m_{2000.0};
    double min_relative_altitude_m_{30.0};
    double max_relative_altitude_m_{120.0};
    double ack_timeout_s_{2.0};
    double confirmation_timeout_s_{3.0};
    double setpoint_horizontal_tolerance_m_{5.0};
    double setpoint_altitude_tolerance_m_{2.0};
    double arrival_horizontal_threshold_m_{100.0};
    double arrival_vertical_threshold_m_{15.0};
    double arrival_hold_time_s_{2.0};
    double monitor_rate_hz_{5.0};
    int source_system_id_{1};
    int source_component_id_{191};

    rclcpp::CallbackGroup::SharedPtr callback_group_;
    rclcpp::Publisher<VehicleCommand>::SharedPtr vehicle_command_pub_;
    rclcpp::Subscription<VehicleGlobalPosition>::SharedPtr global_position_sub_;
    rclcpp::Subscription<SensorGps>::SharedPtr gps_sub_;
    rclcpp::Subscription<HomePosition>::SharedPtr home_position_sub_;
    rclcpp::Subscription<VehicleStatus>::SharedPtr vehicle_status_sub_;
    rclcpp::Subscription<VehicleLandDetected>::SharedPtr land_detected_sub_;
    rclcpp::Subscription<FailsafeFlags>::SharedPtr failsafe_flags_sub_;
    rclcpp::Subscription<VehicleCommandAck>::SharedPtr command_ack_sub_;
    rclcpp::Subscription<PositionSetpointTriplet>::SharedPtr position_setpoint_triplet_sub_;
    rclcpp::Service<GotoGlobal>::SharedPtr goto_global_srv_;
    rclcpp::Service<Trigger>::SharedPtr status_srv_;
    rclcpp::Service<Trigger>::SharedPtr rtl_srv_;
    rclcpp::TimerBase::SharedPtr monitor_timer_;

    std::mutex data_mutex_;
    std::condition_variable data_cv_;
    std::mutex command_mutex_;
    std::atomic<uint32_t> rtl_requests_pending_{0U};
    std::atomic<bool> rtl_callback_active_{false};

    TimedTelemetry<VehicleGlobalPosition> global_position_;
    TimedTelemetry<SensorGps> gps_;
    TimedTelemetry<HomePosition> home_position_;
    TimedTelemetry<VehicleStatus> vehicle_status_;
    TimedTelemetry<VehicleLandDetected> land_detected_;
    TimedTelemetry<FailsafeFlags> failsafe_flags_;
    TimedTelemetry<PositionSetpointTriplet> position_setpoint_triplet_;

    uint64_t ack_generation_{0};
    std::deque<AckRecord> ack_records_;
    GotoState state_{GotoState::Idle};
    std::optional<Target> active_target_;
    std::optional<SteadyTime> arrival_condition_since_;
    std::string last_detail_{"No goto command has been accepted."};

    void validate_parameters() const
    {
        const auto require_positive = [](double value, const char * name) {
                if (!std::isfinite(value) || value <= 0.0) {
                    throw std::invalid_argument(std::string(name) + " must be finite and > 0");
                }
            };

        require_positive(telemetry_timeout_s_, "telemetry_timeout_s");
        require_positive(land_detected_timeout_s_, "land_detected_timeout_s");
        require_positive(gps_max_horizontal_accuracy_m_, "gps_max_horizontal_accuracy_m");
        require_positive(gps_max_vertical_accuracy_m_, "gps_max_vertical_accuracy_m");
        require_positive(max_target_distance_m_, "max_target_distance_m");
        require_positive(ack_timeout_s_, "ack_timeout_s");
        require_positive(confirmation_timeout_s_, "confirmation_timeout_s");
        require_positive(setpoint_horizontal_tolerance_m_, "setpoint_horizontal_tolerance_m");
        require_positive(setpoint_altitude_tolerance_m_, "setpoint_altitude_tolerance_m");
        require_positive(arrival_horizontal_threshold_m_, "arrival_horizontal_threshold_m");
        require_positive(arrival_vertical_threshold_m_, "arrival_vertical_threshold_m");
        require_positive(arrival_hold_time_s_, "arrival_hold_time_s");
        require_positive(monitor_rate_hz_, "monitor_rate_hz");

        if (!std::isfinite(min_relative_altitude_m_) ||
            !std::isfinite(max_relative_altitude_m_) ||
            min_relative_altitude_m_ < 0.0 ||
            max_relative_altitude_m_ <= min_relative_altitude_m_)
        {
            throw std::invalid_argument(
                      "relative altitude limits must satisfy 0 <= min_relative_altitude_m < max_relative_altitude_m");
        }

        if (gps_min_fix_type_ < SensorGps::FIX_TYPE_3D ||
            gps_min_fix_type_ > SensorGps::FIX_TYPE_RTK_FIXED)
        {
            throw std::invalid_argument("gps_min_fix_type must be between 3 and 6");
        }

        if (gps_min_satellites_ < 1 || gps_min_satellites_ > 255) {
            throw std::invalid_argument("gps_min_satellites must be between 1 and 255");
        }

        if (source_system_id_ < 1 || source_system_id_ > 255) {
            throw std::invalid_argument("source_system_id must be between 1 and 255");
        }

        if (source_component_id_ < 1 || source_component_id_ > 65535) {
            throw std::invalid_argument("source_component_id must be between 1 and 65535");
        }
    }

    template<typename MessageT>
    void store_telemetry(TimedTelemetry<MessageT> & destination, const MessageT & message)
    {
        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            destination.message = message;
            destination.received_at = SteadyClock::now();
            destination.received = true;
            ++destination.generation;
        }
        data_cv_.notify_all();
    }

    void command_ack_callback(const VehicleCommandAck::SharedPtr msg)
    {
        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            ++ack_generation_;
            ack_records_.push_back(AckRecord{ack_generation_, *msg});
            while (ack_records_.size() > 64U) {
                ack_records_.pop_front();
            }
        }
        data_cv_.notify_all();
    }

    template<typename MessageT>
    bool telemetry_is_fresh_locked(
        const TimedTelemetry<MessageT> & telemetry,
        const SteadyTime now) const
    {
        if (!telemetry.received) {
            return false;
        }
        return std::chrono::duration<double>(now - telemetry.received_at).count() <=
               telemetry_timeout_s_;
    }

    template<typename MessageT>
    bool require_fresh_locked(
        const TimedTelemetry<MessageT> & telemetry,
        const char * name,
        const SteadyTime now,
        std::string & error,
        const double timeout_s = -1.0) const
    {
        if (!telemetry.received) {
            error = std::string("Missing telemetry: ") + name;
            return false;
        }

        const double age_s =
            std::chrono::duration<double>(now - telemetry.received_at).count();
        const double effective_timeout_s = timeout_s > 0.0 ? timeout_s : telemetry_timeout_s_;
        if (age_s > effective_timeout_s) {
            std::ostringstream stream;
            stream << "Stale telemetry: " << name << " age=" << std::fixed
                   << std::setprecision(2) << age_s << "s";
            error = stream.str();
            return false;
        }
        return true;
    }

    std::optional<std::string> blocking_failsafe_reason_locked() const
    {
        const auto & status = vehicle_status_.message;
        const auto & flags = failsafe_flags_.message;

        if (status.failsafe || status.failsafe_and_user_took_over) {
            return "PX4 reports active failsafe";
        }
        if (status.failsafe_defer_state == VehicleStatus::FAILSAFE_DEFER_STATE_WOULD_FAILSAFE) {
            return "PX4 reports a deferred failsafe condition";
        }
        if (status.failure_detector_status != VehicleStatus::FAILURE_NONE) {
            return "PX4 failure detector is active";
        }
        if (flags.angular_velocity_invalid || flags.attitude_invalid) {
            return "Attitude or angular velocity estimate is invalid";
        }
        if (flags.local_altitude_invalid || flags.local_position_invalid ||
            flags.local_velocity_invalid)
        {
            return "Local position, velocity, or altitude estimate is invalid";
        }
        if (flags.global_position_invalid) {
            return "Global position estimate is invalid";
        }
        if (flags.home_position_invalid) {
            return "PX4 home position is invalid";
        }
        if (flags.manual_control_signal_lost) {
            return "Manual/RC control link is lost";
        }
        if (flags.battery_warning != 0U || flags.battery_low_remaining_time ||
            flags.battery_unhealthy)
        {
            return "Battery failsafe or warning is active";
        }
        if (flags.geofence_breached) {
            return "Geofence is currently breached";
        }
        if (flags.mission_failure) {
            return "Mission failure is active";
        }
        if (flags.vtol_fixed_wing_system_failure) {
            return "VTOL fixed-wing system failure is active";
        }
        if (flags.wind_limit_exceeded || flags.flight_time_limit_exceeded) {
            return "Wind or flight-time limit is exceeded";
        }
        if (flags.local_position_accuracy_low) {
            return "PX4 reports low local-position accuracy";
        }
        if (flags.fd_critical_failure || flags.fd_esc_arming_failure ||
            flags.fd_imbalanced_prop || flags.fd_motor_failure)
        {
            return "Critical, ESC, propeller, or motor failure is active";
        }
        return std::nullopt;
    }

    bool validate_common_safety_locked(const SteadyTime now, std::string & error) const
    {
        if (!require_fresh_locked(global_position_, "vehicle_global_position", now, error) ||
            !require_fresh_locked(gps_, "vehicle_gps_position", now, error) ||
            !require_fresh_locked(vehicle_status_, "vehicle_status", now, error) ||
            !require_fresh_locked(
                land_detected_,
                "vehicle_land_detected",
                now,
                error,
                land_detected_timeout_s_) ||
            !require_fresh_locked(failsafe_flags_, "failsafe_flags", now, error))
        {
            return false;
        }

        if (!home_position_.received) {
            error = "Missing telemetry: home_position";
            return false;
        }

        const auto & global = global_position_.message;
        const auto & gps = gps_.message;
        const auto & home = home_position_.message;
        const auto & status = vehicle_status_.message;
        const auto & landed = land_detected_.message;

        if (status.vehicle_type != VehicleStatus::VEHICLE_TYPE_FIXED_WING) {
            error = "Vehicle is not currently in fixed-wing configuration";
            return false;
        }
        if (status.in_transition_mode) {
            error = "Vehicle is in VTOL transition";
            return false;
        }
        if (status.arming_state != VehicleStatus::ARMING_STATE_ARMED) {
            error = "Vehicle is not armed; this node never arms the vehicle";
            return false;
        }
        if (landed.landed || landed.maybe_landed || landed.ground_contact) {
            error = "Vehicle is not confirmed airborne; this node never takes off";
            return false;
        }
        if (landed.freefall) {
            error = "Vehicle is in freefall";
            return false;
        }
        if (status.nav_state == VehicleStatus::NAVIGATION_STATE_AUTO_RTL ||
            status.nav_state == VehicleStatus::NAVIGATION_STATE_AUTO_TAKEOFF ||
            status.nav_state == VehicleStatus::NAVIGATION_STATE_AUTO_LAND ||
            status.nav_state == VehicleStatus::NAVIGATION_STATE_TERMINATION ||
            status.nav_state == VehicleStatus::NAVIGATION_STATE_DESCEND)
        {
            error =
                "PX4 is in RTL, takeoff, landing, descend, or termination; "
                "change mode deliberately before requesting a new goto";
            return false;
        }

        if (const auto failsafe_reason = blocking_failsafe_reason_locked()) {
            error = *failsafe_reason;
            return false;
        }

        // PX4 v1.15 VehicleGlobalPosition does not expose the lat_lon_valid or
        // alt_valid fields added by later message schemas. FailsafeFlags above is
        // the v1.15 validity source; retain finite/range and dead-reckoning checks
        // here as an independent structural guard.
        if (global.dead_reckoning || !valid_latitude(global.lat) ||
            !valid_longitude(global.lon) || !std::isfinite(global.alt))
        {
            error = "Fused global position is invalid or dead-reckoned";
            return false;
        }

        if (!home.valid_hpos || !home.valid_alt ||
            !valid_latitude(home.lat) || !valid_longitude(home.lon) ||
            !std::isfinite(home.alt))
        {
            error = "Home latitude, longitude, or AMSL altitude is invalid";
            return false;
        }

        if (gps.fix_type == SensorGps::FIX_TYPE_EXTRAPOLATED ||
            gps.fix_type < static_cast<uint8_t>(gps_min_fix_type_) ||
            gps.fix_type > SensorGps::FIX_TYPE_RTK_FIXED)
        {
            std::ostringstream stream;
            stream << "GPS fix_type=" << static_cast<int>(gps.fix_type)
                   << " does not satisfy configured minimum=" << gps_min_fix_type_;
            error = stream.str();
            return false;
        }
        if (gps.satellites_used < static_cast<uint8_t>(gps_min_satellites_)) {
            std::ostringstream stream;
            stream << "GPS satellites=" << static_cast<int>(gps.satellites_used)
                   << " is below minimum=" << gps_min_satellites_;
            error = stream.str();
            return false;
        }
        // PX4 v1.15 SensorGps uses WARNING/CRITICAL and
        // INDICATED/MULTIPLE enums. It does not yet expose system_error or
        // authentication_state, so those later-schema checks cannot be made.
        if (gps.jamming_state == SensorGps::JAMMING_STATE_WARNING ||
            gps.jamming_state == SensorGps::JAMMING_STATE_CRITICAL)
        {
            error = "GPS jamming warning or critical state is active";
            return false;
        }
        if (gps.spoofing_state == SensorGps::SPOOFING_STATE_INDICATED ||
            gps.spoofing_state == SensorGps::SPOOFING_STATE_MULTIPLE)
        {
            error = "GPS spoofing is detected";
            return false;
        }

        if (!accuracy_is_acceptable(gps.eph, gps_max_horizontal_accuracy_m_) ||
            !accuracy_is_acceptable(global.eph, gps_max_horizontal_accuracy_m_))
        {
            error = "GPS/fused horizontal accuracy is outside the configured limit";
            return false;
        }
        if (!accuracy_is_acceptable(gps.epv, gps_max_vertical_accuracy_m_) ||
            !accuracy_is_acceptable(global.epv, gps_max_vertical_accuracy_m_))
        {
            error = "GPS/fused vertical accuracy is outside the configured limit";
            return false;
        }

        return true;
    }

    bool prepare_target_locked(
        const GotoGlobal::Request & request,
        const SteadyTime now,
        Target & target,
        std::string & error) const
    {
        if (!validate_common_safety_locked(now, error)) {
            return false;
        }

        if (!valid_latitude(request.latitude_deg) ||
            !valid_longitude(request.longitude_deg))
        {
            error = "Target latitude or longitude is outside WGS84 bounds";
            return false;
        }
        if (!std::isfinite(request.altitude_m)) {
            error = "Target altitude must be finite";
            return false;
        }

        const auto & home = home_position_.message;
        const auto & current = global_position_.message;

        target.latitude_deg = request.latitude_deg;
        target.longitude_deg = request.longitude_deg;

        if (request.altitude_reference == GotoGlobal::Request::ALTITUDE_REFERENCE_AMSL) {
            target.altitude_amsl_m = request.altitude_m;
            target.altitude_relative_home_m = target.altitude_amsl_m - home.alt;
        } else if (
            request.altitude_reference ==
            GotoGlobal::Request::ALTITUDE_REFERENCE_RELATIVE_HOME)
        {
            target.altitude_relative_home_m = request.altitude_m;
            target.altitude_amsl_m = home.alt + request.altitude_m;
        } else {
            error = "Unknown altitude_reference; use AMSL=0 or RELATIVE_HOME=1";
            return false;
        }

        if (!std::isfinite(target.altitude_amsl_m) ||
            !std::isfinite(target.altitude_relative_home_m))
        {
            error = "Converted target altitude is not finite";
            return false;
        }
        if (target.altitude_relative_home_m < min_relative_altitude_m_ ||
            target.altitude_relative_home_m > max_relative_altitude_m_)
        {
            std::ostringstream stream;
            stream << "Target relative altitude=" << std::fixed << std::setprecision(1)
                   << target.altitude_relative_home_m << "m is outside ["
                   << min_relative_altitude_m_ << ", " << max_relative_altitude_m_ << "]m";
            error = stream.str();
            return false;
        }

        target.initial_distance_m = great_circle_distance_m(
            current.lat,
            current.lon,
            target.latitude_deg,
            target.longitude_deg);
        if (!std::isfinite(target.initial_distance_m) ||
            target.initial_distance_m > max_target_distance_m_)
        {
            std::ostringstream stream;
            stream << "Target distance=" << std::fixed << std::setprecision(1)
                   << target.initial_distance_m << "m exceeds maximum="
                   << max_target_distance_m_ << "m";
            error = stream.str();
            return false;
        }

        target.home_distance_m = great_circle_distance_m(
            home.lat,
            home.lon,
            target.latitude_deg,
            target.longitude_deg);
        if (!std::isfinite(target.home_distance_m) ||
            target.home_distance_m > max_target_distance_m_)
        {
            std::ostringstream stream;
            stream << "Target distance from Home=" << std::fixed << std::setprecision(1)
                   << target.home_distance_m << "m exceeds maximum="
                   << max_target_distance_m_ << "m";
            error = stream.str();
            return false;
        }

        target.target_system = vehicle_status_.message.system_id;
        target.target_component = vehicle_status_.message.component_id;
        if (target.target_system == 0U || target.target_component == 0U) {
            error = "PX4 system/component ID is invalid";
            return false;
        }
        return true;
    }

    void goto_global_callback(
        const std::shared_ptr<GotoGlobal::Request> request,
        std::shared_ptr<GotoGlobal::Response> response)
    {
        std::unique_lock<std::mutex> command_lock(command_mutex_, std::try_to_lock);
        if (!command_lock.owns_lock()) {
            response->success = false;
            response->message = "Another PX4 command is still pending";
            return;
        }

        Target target;
        uint64_t setpoint_generation_before_command = 0;
        std::string error;
        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            if (!prepare_target_locked(*request, SteadyClock::now(), target, error)) {
                response->success = false;
                response->message = error;
                return;
            }

            setpoint_generation_before_command = position_setpoint_triplet_.generation;
            state_ = GotoState::CommandPending;
            active_target_ = target;
            arrival_condition_since_.reset();
            last_detail_ = "Waiting for PX4 reposition acknowledgement and target confirmation.";
        }

        VehicleCommand command = make_vehicle_command(
            VehicleCommand::VEHICLE_CMD_DO_REPOSITION,
            target.target_system,
            target.target_component);
        command.param1 = -1.0F;
        command.param2 = 1.0F;
        command.param3 = std::numeric_limits<float>::quiet_NaN();
        command.param4 = std::numeric_limits<float>::quiet_NaN();
        command.param5 = target.latitude_deg;
        command.param6 = target.longitude_deg;
        command.param7 = static_cast<float>(target.altitude_amsl_m);

        RCLCPP_WARN(
            this->get_logger(),
            "Requesting PX4 fixed-wing reposition to lat=%.8f lon=%.8f alt_amsl=%.1fm (relative_home=%.1fm).",
            target.latitude_deg,
            target.longitude_deg,
            target.altitude_amsl_m,
            target.altitude_relative_home_m);

        const AckWaitResult ack = publish_and_wait_for_ack(command, true);
        if (ack.preempted) {
            mark_aborted("GOTO was preempted by an RTL/ABORT request");
            response->success = false;
            response->message = "GOTO preempted by RTL/ABORT";
            return;
        }
        if (!ack.received) {
            mark_aborted("Timed out waiting for PX4 DO_REPOSITION acknowledgement");
            response->success = false;
            response->message = "DO_REPOSITION ACK timeout; command outcome is uncertain";
            return;
        }
        if (ack.result != VehicleCommandAck::VEHICLE_CMD_RESULT_ACCEPTED) {
            std::ostringstream stream;
            stream << "PX4 rejected DO_REPOSITION: " << ack_result_name(ack.result);
            mark_aborted(stream.str());
            response->success = false;
            response->message = stream.str();
            return;
        }

        std::string confirmation_error;
        if (!wait_for_goto_confirmation(
                target,
                setpoint_generation_before_command,
                confirmation_error))
        {
            mark_aborted(confirmation_error);
            response->success = false;
            response->message = confirmation_error;
            return;
        }

        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            state_ = GotoState::Enroute;
            active_target_ = target;
            arrival_condition_since_.reset();
            last_detail_ = "PX4 accepted the target, entered AUTO_LOITER, and published the matching loiter setpoint.";
        }

        std::ostringstream stream;
        stream << std::fixed << std::setprecision(7)
               << "GOTO accepted lat=" << target.latitude_deg
               << " lon=" << target.longitude_deg << std::setprecision(1)
               << " alt_amsl=" << target.altitude_amsl_m
               << "m alt_relative_home=" << target.altitude_relative_home_m
               << "m distance=" << target.initial_distance_m << "m";
        response->success = true;
        response->message = stream.str();
    }

    void rtl_callback(
        const std::shared_ptr<Trigger::Request> request,
        std::shared_ptr<Trigger::Response> response)
    {
        (void)request;
        bool expected_inactive = false;
        if (!rtl_callback_active_.compare_exchange_strong(
                expected_inactive, true, std::memory_order_acq_rel))
        {
            response->success = false;
            response->message = "An RTL/ABORT request is already pending";
            return;
        }
        RtlCallbackGuard rtl_callback_guard(rtl_callback_active_);

        // Wake an in-flight GOTO wait first, then serialize RTL behind the short cleanup
        // path. If RTL wins the mutex race, a later GOTO is rejected while PX4 is in AUTO_RTL.
        RtlRequestGuard rtl_request_guard(rtl_requests_pending_);
        data_cv_.notify_all();
        std::unique_lock<std::mutex> command_lock(command_mutex_);

        uint8_t target_system = 0;
        uint8_t target_component = 0;
        std::string error;
        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            if (!validate_rtl_request_locked(SteadyClock::now(), error)) {
                response->success = false;
                response->message = error;
                return;
            }
            target_system = vehicle_status_.message.system_id;
            target_component = vehicle_status_.message.component_id;
        }

        VehicleCommand command = make_vehicle_command(
            VehicleCommand::VEHICLE_CMD_NAV_RETURN_TO_LAUNCH,
            target_system,
            target_component);

        RCLCPP_WARN(this->get_logger(), "Requesting PX4 Return mode. This node will not land the vehicle.");
        const AckWaitResult ack = publish_and_wait_for_ack(command);
        if (!ack.received) {
            response->success = false;
            response->message = "RTL ACK timeout; command outcome is uncertain";
            return;
        }
        if (ack.result != VehicleCommandAck::VEHICLE_CMD_RESULT_ACCEPTED) {
            std::ostringstream stream;
            stream << "PX4 rejected RTL: " << ack_result_name(ack.result);
            response->success = false;
            response->message = stream.str();
            return;
        }

        std::string confirmation_error;
        if (!wait_for_navigation_state(
                VehicleStatus::NAVIGATION_STATE_AUTO_RTL,
                confirmation_error))
        {
            mark_aborted(confirmation_error);
            response->success = false;
            response->message = confirmation_error;
            return;
        }

        mark_aborted("Goto tracking stopped because PX4 confirmed AUTO_RTL.");
        response->success = true;
        response->message = "PX4 accepted RTL and entered AUTO_RTL; landing remains under PX4/pilot control";
    }

    void status_callback(
        const std::shared_ptr<Trigger::Request> request,
        std::shared_ptr<Trigger::Response> response)
    {
        (void)request;
        std::lock_guard<std::mutex> lock(data_mutex_);
        const auto now = SteadyClock::now();
        std::string readiness_error;
        const bool ready = validate_common_safety_locked(now, readiness_error);

        std::ostringstream stream;
        stream << "state=" << state_name(state_)
               << ";ready_for_goto=" << (ready ? "true" : "false")
               << ";detail=" << last_detail_;

        if (!ready) {
            stream << ";readiness_reason=" << readiness_error;
        }
        if (vehicle_status_.received) {
            stream << ";nav_state=" << static_cast<int>(vehicle_status_.message.nav_state)
                   << ";armed="
                   << (vehicle_status_.message.arming_state == VehicleStatus::ARMING_STATE_ARMED ?
                       "true" : "false")
                   << ";fixed_wing="
                   << (vehicle_status_.message.vehicle_type == VehicleStatus::VEHICLE_TYPE_FIXED_WING ?
                       "true" : "false");
        }
        if (land_detected_.received) {
            stream << ";landed=" << (land_detected_.message.landed ? "true" : "false");
        }
        if (gps_.received) {
            stream << ";gps_fix=" << static_cast<int>(gps_.message.fix_type)
                   << ";satellites=" << static_cast<int>(gps_.message.satellites_used)
                   << ";gps_eph_m=" << std::fixed << std::setprecision(1) << gps_.message.eph
                   << ";gps_epv_m=" << gps_.message.epv;
        }
        if (active_target_) {
            stream << std::fixed << std::setprecision(7)
                   << ";target_lat=" << active_target_->latitude_deg
                   << ";target_lon=" << active_target_->longitude_deg
                   << std::setprecision(1)
                   << ";target_alt_amsl_m=" << active_target_->altitude_amsl_m
                   << ";target_alt_relative_home_m="
                   << active_target_->altitude_relative_home_m
                   << ";target_distance_from_home_m=" << active_target_->home_distance_m;

            if (global_position_.received &&
                !global_position_.message.dead_reckoning &&
                valid_latitude(global_position_.message.lat) &&
                valid_longitude(global_position_.message.lon) &&
                std::isfinite(global_position_.message.alt))
            {
                stream << ";distance_to_target_m="
                       << great_circle_distance_m(
                            global_position_.message.lat,
                            global_position_.message.lon,
                            active_target_->latitude_deg,
                            active_target_->longitude_deg)
                       << ";altitude_error_m="
                       << std::fabs(
                            static_cast<double>(global_position_.message.alt) -
                            active_target_->altitude_amsl_m);
            }
        }

        response->success = true;
        response->message = stream.str();
    }

    VehicleCommand make_vehicle_command(
        const uint32_t command_id,
        const uint8_t target_system,
        const uint8_t target_component)
    {
        VehicleCommand command{};
        const float nan = std::numeric_limits<float>::quiet_NaN();
        command.timestamp = static_cast<uint64_t>(this->get_clock()->now().nanoseconds() / 1000);
        command.param1 = nan;
        command.param2 = nan;
        command.param3 = nan;
        command.param4 = nan;
        command.param5 = std::numeric_limits<double>::quiet_NaN();
        command.param6 = std::numeric_limits<double>::quiet_NaN();
        command.param7 = nan;
        command.command = command_id;
        command.target_system = target_system;
        command.target_component = target_component;
        command.source_system = static_cast<uint8_t>(source_system_id_);
        command.source_component = static_cast<uint16_t>(source_component_id_);
        command.confirmation = 0;
        command.from_external = true;
        return command;
    }

    bool validate_rtl_request_locked(const SteadyTime now, std::string & error) const
    {
        // RTL is an operator escape path. Do not block it on GPS quality, battery, RC,
        // failsafe, or the current goto state; PX4 remains the authority that accepts or
        // rejects the command according to its own Return requirements and configuration.
        if (!require_fresh_locked(vehicle_status_, "vehicle_status", now, error)) {
            return false;
        }

        const auto & status = vehicle_status_.message;
        if (status.system_id == 0U || status.component_id == 0U) {
            error = "PX4 system/component ID is invalid";
            return false;
        }
        return true;
    }

    AckWaitResult publish_and_wait_for_ack(
        const VehicleCommand & command,
        const bool allow_goto_preemption = false)
    {
        uint64_t baseline_generation = 0;
        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            baseline_generation = ack_generation_;
        }

        if (allow_goto_preemption &&
            rtl_requests_pending_.load(std::memory_order_acquire) > 0U)
        {
            return AckWaitResult{false, VehicleCommandAck::VEHICLE_CMD_RESULT_FAILED, true};
        }

        vehicle_command_pub_->publish(command);

        const auto deadline = SteadyClock::now() +
            std::chrono::duration_cast<SteadyClock::duration>(
            std::chrono::duration<double>(ack_timeout_s_));
        std::unique_lock<std::mutex> lock(data_mutex_);

        while (true) {
            if (allow_goto_preemption &&
                rtl_requests_pending_.load(std::memory_order_acquire) > 0U)
            {
                return AckWaitResult{
                    false, VehicleCommandAck::VEHICLE_CMD_RESULT_FAILED, true};
            }

            for (const auto & record : ack_records_) {
                const auto & ack = record.message;
                if (record.generation <= baseline_generation ||
                    ack.command != command.command ||
                    ack.target_system != command.source_system ||
                    ack.target_component != command.source_component)
                {
                    continue;
                }

                if (ack.result != VehicleCommandAck::VEHICLE_CMD_RESULT_IN_PROGRESS) {
                    return AckWaitResult{true, ack.result};
                }
            }

            if (data_cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                return AckWaitResult{};
            }
        }
    }

    bool wait_for_goto_confirmation(
        const Target & target,
        const uint64_t setpoint_generation_before_command,
        std::string & error)
    {
        const auto deadline = SteadyClock::now() +
            std::chrono::duration_cast<SteadyClock::duration>(
            std::chrono::duration<double>(confirmation_timeout_s_));
        std::unique_lock<std::mutex> lock(data_mutex_);

        while (true) {
            if (rtl_requests_pending_.load(std::memory_order_acquire) > 0U) {
                error = "GOTO confirmation was preempted by RTL/ABORT";
                return false;
            }

            const auto now = SteadyClock::now();
            const bool mode_confirmed =
                telemetry_is_fresh_locked(vehicle_status_, now) &&
                vehicle_status_.message.nav_state == VehicleStatus::NAVIGATION_STATE_AUTO_LOITER;
            const bool setpoint_confirmed =
                position_setpoint_triplet_.received &&
                position_setpoint_triplet_.generation > setpoint_generation_before_command &&
                target_matches_setpoint_locked(target);

            if (mode_confirmed && setpoint_confirmed) {
                return true;
            }

            if (data_cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                std::ostringstream stream;
                stream << "PX4 accepted DO_REPOSITION but confirmation timed out: AUTO_LOITER="
                       << (mode_confirmed ? "true" : "false")
                       << ",matching_target_setpoint="
                       << (setpoint_confirmed ? "true" : "false")
                       << ". The geofence or Navigator may have rejected the target.";
                error = stream.str();
                return false;
            }
        }
    }

    bool wait_for_navigation_state(const uint8_t desired_state, std::string & error)
    {
        const auto deadline = SteadyClock::now() +
            std::chrono::duration_cast<SteadyClock::duration>(
            std::chrono::duration<double>(confirmation_timeout_s_));
        std::unique_lock<std::mutex> lock(data_mutex_);

        while (true) {
            const auto now = SteadyClock::now();
            if (telemetry_is_fresh_locked(vehicle_status_, now) &&
                vehicle_status_.message.nav_state == desired_state)
            {
                return true;
            }

            if (data_cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                std::ostringstream stream;
                stream << "PX4 accepted the command but nav_state did not become "
                       << static_cast<int>(desired_state) << " before timeout";
                error = stream.str();
                return false;
            }
        }
    }

    bool target_matches_setpoint_locked(const Target & target) const
    {
        if (!position_setpoint_triplet_.received) {
            return false;
        }

        const auto & setpoint = position_setpoint_triplet_.message.current;
        if (!setpoint.valid || setpoint.type != PositionSetpoint::SETPOINT_TYPE_LOITER ||
            !valid_latitude(setpoint.lat) || !valid_longitude(setpoint.lon) ||
            !std::isfinite(setpoint.alt))
        {
            return false;
        }

        const double horizontal_error_m = great_circle_distance_m(
            setpoint.lat,
            setpoint.lon,
            target.latitude_deg,
            target.longitude_deg);
        const double vertical_error_m =
            std::fabs(static_cast<double>(setpoint.alt) - target.altitude_amsl_m);
        return horizontal_error_m <= setpoint_horizontal_tolerance_m_ &&
               vertical_error_m <= setpoint_altitude_tolerance_m_;
    }

    void monitor_callback()
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        if (state_ != GotoState::Enroute && state_ != GotoState::Arrived) {
            return;
        }
        if (!active_target_) {
            abort_tracking_locked("Active target disappeared from goto state");
            return;
        }

        const auto now = SteadyClock::now();
        std::string health_error;
        if (!validate_common_safety_locked(now, health_error)) {
            abort_tracking_locked("Goto monitoring stopped: " + health_error);
            return;
        }

        if (vehicle_status_.message.nav_state != VehicleStatus::NAVIGATION_STATE_AUTO_LOITER) {
            std::ostringstream stream;
            stream << "Goto aborted because PX4 mode was overridden; nav_state="
                   << static_cast<int>(vehicle_status_.message.nav_state);
            abort_tracking_locked(stream.str());
            return;
        }
        if (!target_matches_setpoint_locked(*active_target_)) {
            abort_tracking_locked(
                "Goto aborted because PX4 loiter target no longer matches the commanded target");
            return;
        }

        const double horizontal_distance_m = great_circle_distance_m(
            global_position_.message.lat,
            global_position_.message.lon,
            active_target_->latitude_deg,
            active_target_->longitude_deg);
        const double vertical_error_m = std::fabs(
            static_cast<double>(global_position_.message.alt) -
            active_target_->altitude_amsl_m);
        const bool inside_arrival_region =
            horizontal_distance_m <= arrival_horizontal_threshold_m_ &&
            vertical_error_m <= arrival_vertical_threshold_m_;

        if (state_ == GotoState::Enroute) {
            if (inside_arrival_region) {
                if (!arrival_condition_since_) {
                    arrival_condition_since_ = now;
                } else if (
                    std::chrono::duration<double>(now - *arrival_condition_since_).count() >=
                    arrival_hold_time_s_)
                {
                    state_ = GotoState::Arrived;
                    std::ostringstream stream;
                    stream << "Arrival thresholds held for " << std::fixed
                           << std::setprecision(1) << arrival_hold_time_s_
                           << "s; distance=" << horizontal_distance_m
                           << "m altitude_error=" << vertical_error_m << "m";
                    last_detail_ = stream.str();
                    RCLCPP_INFO(this->get_logger(), "%s", last_detail_.c_str());
                }
            } else {
                arrival_condition_since_.reset();
            }
        }
    }

    void mark_aborted(const std::string & reason)
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        abort_locked(reason);
    }

    void abort_locked(const std::string & reason)
    {
        state_ = GotoState::Aborted;
        arrival_condition_since_.reset();
        last_detail_ = reason;
        RCLCPP_WARN(this->get_logger(), "%s", reason.c_str());
    }

    void abort_tracking_locked(const std::string & reason)
    {
        abort_locked(
            reason + "; this monitor only stopped node-side tracking and did not change PX4 mode");
    }

    static bool valid_latitude(const double latitude_deg)
    {
        return std::isfinite(latitude_deg) && latitude_deg >= -90.0 && latitude_deg <= 90.0;
    }

    static bool valid_longitude(const double longitude_deg)
    {
        return std::isfinite(longitude_deg) && longitude_deg >= -180.0 && longitude_deg <= 180.0;
    }

    static bool accuracy_is_acceptable(const float accuracy_m, const double limit_m)
    {
        return std::isfinite(accuracy_m) && accuracy_m >= 0.0F && accuracy_m <= limit_m;
    }

    static double great_circle_distance_m(
        const double latitude_a_deg,
        const double longitude_a_deg,
        const double latitude_b_deg,
        const double longitude_b_deg)
    {
        constexpr double pi = 3.14159265358979323846;
        constexpr double earth_radius_m = 6371000.0;
        const double latitude_a = latitude_a_deg * pi / 180.0;
        const double latitude_b = latitude_b_deg * pi / 180.0;
        const double delta_latitude = (latitude_b_deg - latitude_a_deg) * pi / 180.0;
        const double delta_longitude = (longitude_b_deg - longitude_a_deg) * pi / 180.0;
        const double sin_latitude = std::sin(delta_latitude / 2.0);
        const double sin_longitude = std::sin(delta_longitude / 2.0);
        const double haversine = sin_latitude * sin_latitude +
            std::cos(latitude_a) * std::cos(latitude_b) *
            sin_longitude * sin_longitude;
        return 2.0 * earth_radius_m *
               std::asin(std::sqrt(std::clamp(haversine, 0.0, 1.0)));
    }

    static const char * state_name(const GotoState state)
    {
        switch (state) {
        case GotoState::Idle:
            return "IDLE";
        case GotoState::CommandPending:
            return "COMMAND_PENDING";
        case GotoState::Enroute:
            return "ENROUTE";
        case GotoState::Arrived:
            return "ARRIVED";
        case GotoState::Aborted:
            return "ABORTED";
        }
        return "UNKNOWN";
    }

    static const char * ack_result_name(const uint8_t result)
    {
        switch (result) {
        case VehicleCommandAck::VEHICLE_CMD_RESULT_ACCEPTED:
            return "ACCEPTED";
        case VehicleCommandAck::VEHICLE_CMD_RESULT_TEMPORARILY_REJECTED:
            return "TEMPORARILY_REJECTED";
        case VehicleCommandAck::VEHICLE_CMD_RESULT_DENIED:
            return "DENIED";
        case VehicleCommandAck::VEHICLE_CMD_RESULT_UNSUPPORTED:
            return "UNSUPPORTED";
        case VehicleCommandAck::VEHICLE_CMD_RESULT_FAILED:
            return "FAILED";
        case VehicleCommandAck::VEHICLE_CMD_RESULT_IN_PROGRESS:
            return "IN_PROGRESS";
        case VehicleCommandAck::VEHICLE_CMD_RESULT_CANCELLED:
            return "CANCELLED";
        default:
            return "UNKNOWN";
        }
    }
};

int main(int argc, char * argv[])
{
    rclcpp::init(argc, argv);
    auto node = std::make_shared<GlobalGotoNode>();
    rclcpp::executors::MultiThreadedExecutor executor(rclcpp::ExecutorOptions(), 4U);
    executor.add_node(node);
    executor.spin();
    rclcpp::shutdown();
    return 0;
}
