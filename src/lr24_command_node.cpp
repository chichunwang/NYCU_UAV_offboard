#include <rclcpp/rclcpp.hpp>

#include <my_offboard_cpp/srv/goto_global.hpp>
#include <std_srvs/srv/trigger.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <fcntl.h>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;

class Lr24CommandNode : public rclcpp::Node
{
public:
    Lr24CommandNode() : Node("lr24_command_node")
    {
        port_ = this->declare_parameter<std::string>("port", "/dev/ttyUSB0");
        baud_rate_ = this->declare_parameter<int>("baud_rate", 115200);
        service_response_timeout_s_ =
            this->declare_parameter<double>("service_response_timeout_s", 7.0);
        if (!std::isfinite(service_response_timeout_s_) || service_response_timeout_s_ <= 0.0) {
            throw std::invalid_argument("service_response_timeout_s must be finite and > 0");
        }

        const std::string return_to_launch_service =
            this->declare_parameter<std::string>(
            "return_to_launch_service", "/return_to_launch");

        trigger_service_names_ = {
            {"START_OFFBOARD", this->declare_parameter<std::string>("start_offboard_service", "/start_offboard")},
            {"ENABLE_STREAM", this->declare_parameter<std::string>("enable_stream_service", "/enable_offboard_stream")},
            {"START_MISSION", this->declare_parameter<std::string>("start_mission_service", "/start_mission")},
            {"STOP_OFFBOARD", this->declare_parameter<std::string>("stop_offboard_service", "/stop_offboard")},
            {"LAND", this->declare_parameter<std::string>("land_service", "/land")},
            {"STATUS", this->declare_parameter<std::string>("status_service", "/offboard_status")},
            {"RTL", return_to_launch_service},
            {"ABORT", return_to_launch_service},
        };

        for (const auto & item : trigger_service_names_) {
            trigger_clients_[item.first] =
                this->create_client<std_srvs::srv::Trigger>(item.second);
        }

        const std::string goto_global_service =
            this->declare_parameter<std::string>("goto_global_service", "/goto_global");
        goto_global_client_ =
            this->create_client<my_offboard_cpp::srv::GotoGlobal>(goto_global_service);

        open_serial();

        timer_ = this->create_wall_timer(
            20ms,
            std::bind(&Lr24CommandNode::timer_callback, this));
    }

    ~Lr24CommandNode() override
    {
        if (serial_fd_ >= 0) {
            close(serial_fd_);
        }
    }

private:
    using Trigger = std_srvs::srv::Trigger;
    using GotoGlobal = my_offboard_cpp::srv::GotoGlobal;

    struct CommandRequest
    {
        std::string sequence{"0"};
        std::string command;
        std::vector<std::string> arguments;
        std::string payload;
        bool framed{false};
    };

    struct PendingMetadata
    {
        std::string sequence;
        std::string command;
        std::string request_payload;
        bool framed{false};
    };

    struct PendingTrigger
    {
        PendingMetadata metadata;
        rclcpp::Client<Trigger>::FutureAndRequestId future;
        std::chrono::steady_clock::time_point deadline;
    };

    struct PendingGoto
    {
        PendingMetadata metadata;
        rclcpp::Client<GotoGlobal>::FutureAndRequestId future;
        std::chrono::steady_clock::time_point deadline;
    };

    struct CachedResponse
    {
        std::string request_payload;
        std::string response_frame;
    };

    static constexpr size_t kDedupCacheLimit = 64;
    static constexpr size_t kMaxLineLength = 256;

    std::string port_;
    int baud_rate_ = 115200;
    double service_response_timeout_s_ = 7.0;
    int serial_fd_ = -1;
    std::string rx_buffer_;
    rclcpp::TimerBase::SharedPtr timer_;
    std::map<std::string, std::string> trigger_service_names_;
    std::map<std::string, rclcpp::Client<Trigger>::SharedPtr> trigger_clients_;
    rclcpp::Client<GotoGlobal>::SharedPtr goto_global_client_;
    std::optional<PendingTrigger> pending_trigger_;
    std::optional<PendingGoto> pending_goto_;
    std::map<std::string, CachedResponse> response_cache_;
    std::deque<std::string> response_cache_order_;

    void timer_callback()
    {
        read_serial();
        poll_pending_services();
    }

    void open_serial()
    {
        serial_fd_ = open(port_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (serial_fd_ < 0) {
            RCLCPP_ERROR(
                this->get_logger(),
                "Failed to open serial port %s: %s",
                port_.c_str(),
                std::strerror(errno));
            return;
        }

        termios tty{};
        if (tcgetattr(serial_fd_, &tty) != 0) {
            RCLCPP_ERROR(this->get_logger(), "tcgetattr failed: %s", std::strerror(errno));
            close(serial_fd_);
            serial_fd_ = -1;
            return;
        }

        cfmakeraw(&tty);
        tty.c_cflag |= CLOCAL | CREAD;
        tty.c_cflag &= ~CRTSCTS;
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~PARENB;
        tty.c_cflag &= ~CSIZE;
        tty.c_cflag |= CS8;
        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = 0;

        speed_t baud = baud_to_constant(baud_rate_);
        cfsetispeed(&tty, baud);
        cfsetospeed(&tty, baud);

        if (tcsetattr(serial_fd_, TCSANOW, &tty) != 0) {
            RCLCPP_ERROR(this->get_logger(), "tcsetattr failed: %s", std::strerror(errno));
            close(serial_fd_);
            serial_fd_ = -1;
            return;
        }

        RCLCPP_INFO(
            this->get_logger(),
            "LR24 command node listening on %s at %d baud.",
            port_.c_str(),
            baud_rate_);

        send_frame("STAT", "0", "BOOT", "LR24 command node ready");
    }

    speed_t baud_to_constant(int baud_rate)
    {
        switch (baud_rate) {
        case 9600:
            return B9600;
        case 19200:
            return B19200;
        case 38400:
            return B38400;
        case 57600:
            return B57600;
        case 115200:
            return B115200;
        case 230400:
            return B230400;
        default:
            RCLCPP_WARN(
                this->get_logger(),
                "Unsupported baud_rate=%d, falling back to 115200.",
                baud_rate);
            return B115200;
        }
    }

    void read_serial()
    {
        if (serial_fd_ < 0) {
            return;
        }

        char buffer[128];
        while (true) {
            ssize_t n = read(serial_fd_, buffer, sizeof(buffer));
            if (n > 0) {
                rx_buffer_.append(buffer, static_cast<size_t>(n));
                consume_lines();
                continue;
            }

            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                RCLCPP_ERROR(this->get_logger(), "Serial read error: %s", std::strerror(errno));
            }
            break;
        }
    }

    void consume_lines()
    {
        size_t pos = std::string::npos;
        while ((pos = rx_buffer_.find('\n')) != std::string::npos) {
            std::string line = rx_buffer_.substr(0, pos);
            rx_buffer_.erase(0, pos + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                if (line.size() > kMaxLineLength) {
                    send_frame("ERR", "0", "FRAME", "frame exceeds 256 bytes");
                    continue;
                }
                if (!is_printable_ascii(line)) {
                    send_frame("ERR", "0", "FRAME", "frame contains non-printable bytes");
                    continue;
                }
                handle_line(line);
            }
        }

        if (rx_buffer_.size() > kMaxLineLength) {
            rx_buffer_.clear();
            send_frame("ERR", "0", "FRAME", "RX buffer overflow");
        }
    }

    void handle_line(const std::string & line)
    {
        CommandRequest request;

        if (!line.empty() && line.front() == '$') {
            std::string error;
            if (!parse_frame(line, request, error)) {
                send_frame("ERR", request.sequence, "FRAME", error);
                return;
            }
        } else {
            const std::string simple_text = uppercase(trim(line));
            const std::string simple_command = first_token(simple_text);
            if (is_flight_critical_command(simple_command)) {
                send_frame(
                    "ERR", "0", simple_command,
                    "flight-critical command requires a checksummed frame");
                return;
            }

            request.command = simple_text;
        }

        if (handle_duplicate_or_conflict(request)) {
            return;
        }

        if (request.command.empty()) {
            send_result(request, false, "empty command");
            return;
        }

        dispatch_command(request);
    }

    bool parse_frame(
        const std::string & line,
        CommandRequest & request,
        std::string & error)
    {
        const size_t star = line.find('*');
        if (star == std::string::npos) {
            error = "missing checksum";
            return false;
        }
        if (star + 3 != line.size()) {
            error = "checksum must be exactly two hex digits at end of frame";
            return false;
        }

        const std::string payload = line.substr(1, star - 1);
        const std::string checksum_text = line.substr(star + 1, 2);
        uint8_t expected = 0;
        if (!parse_hex_byte(checksum_text, expected)) {
            error = "invalid checksum text";
            return false;
        }

        const uint8_t actual = xor_checksum(payload);
        if (actual != expected) {
            std::ostringstream oss;
            oss << "bad checksum expected " << to_hex(actual) << " got " << checksum_text;
            error = oss.str();
            return false;
        }

        const std::vector<std::string> fields = split(payload, ',');
        if (fields.size() < 3 || fields[0] != "CMD") {
            error = "expected CMD,seq,command";
            return false;
        }
        if (fields[1].empty()) {
            error = "sequence must not be empty";
            return false;
        }

        request.sequence = fields[1];
        request.command = uppercase(trim(fields[2]));
        request.arguments.assign(fields.begin() + 3, fields.end());
        request.payload = payload;
        request.framed = true;
        return true;
    }

    bool handle_duplicate_or_conflict(const CommandRequest & request)
    {
        if (!request.framed) {
            return false;
        }

        const auto cached = response_cache_.find(request.sequence);
        if (cached != response_cache_.end()) {
            if (cached->second.request_payload == request.payload) {
                write_serial(cached->second.response_frame);
            } else {
                send_frame(
                    "ERR", request.sequence, request.command,
                    "sequence already used for a different request");
            }
            return true;
        }

        const PendingMetadata * pending = pending_metadata();
        if (pending != nullptr && pending->framed && pending->sequence == request.sequence) {
            if (pending->request_payload != request.payload) {
                send_frame(
                    "ERR", request.sequence, request.command,
                    "sequence already in use for a different request");
            }
            // An identical retransmission is already in flight. The original service completion
            // will emit the response, so it must not execute the command a second time.
            return true;
        }

        return false;
    }

    void dispatch_command(const CommandRequest & request)
    {
        if (request.framed && request.command != "GOTO" &&
            request.command != "GOTO_AMSL" && !request.arguments.empty())
        {
            send_result(request, false, "command does not accept arguments");
            return;
        }

        if (has_pending_service()) {
            if (is_emergency_command(request.command)) {
                const PendingMetadata * pending = pending_metadata();
                if (pending != nullptr && is_emergency_command(pending->command)) {
                    send_result(request, false, "RTL/ABORT is already pending");
                    return;
                }
                cancel_pending_service(
                    "superseded by emergency command " + request.command);
            } else {
                send_result(request, false, "busy waiting for previous ROS service");
                return;
            }
        }

        if (request.command == "PING") {
            send_result(request, true, "PONG");
            return;
        }

        if (request.command == "HELP") {
            send_result(
                request, true,
                "PING ENABLE_STREAM START_MISSION START_OFFBOARD STOP_OFFBOARD LAND RTL ABORT GOTO GOTO_AMSL STATUS");
            return;
        }

        if (request.command == "GOTO" || request.command == "GOTO_AMSL") {
            dispatch_goto(request);
            return;
        }

        const auto client_it = trigger_clients_.find(request.command);
        if (client_it == trigger_clients_.end()) {
            send_result(request, false, "unknown command");
            return;
        }

        const auto client = client_it->second;
        if (!client->wait_for_service(100ms)) {
            send_result(request, false, "ROS service not available");
            return;
        }

        auto service_request = std::make_shared<Trigger::Request>();
        pending_trigger_ = PendingTrigger{
            make_pending_metadata(request),
            client->async_send_request(service_request),
            service_deadline()};
    }

    void dispatch_goto(const CommandRequest & command_request)
    {
        if (!command_request.framed) {
            send_result(
                command_request, false,
                "flight-critical command requires a checksummed frame");
            return;
        }

        if (command_request.arguments.size() != 3) {
            send_result(
                command_request, false,
                "expected CMD,seq,GOTO,latitude,longitude,altitude");
            return;
        }

        double latitude = 0.0;
        double longitude = 0.0;
        double altitude = 0.0;
        std::string error;
        if (!parse_finite_decimal(command_request.arguments[0], "latitude", latitude, error) ||
            !parse_finite_decimal(command_request.arguments[1], "longitude", longitude, error) ||
            !parse_finite_decimal(command_request.arguments[2], "altitude", altitude, error))
        {
            send_result(command_request, false, error);
            return;
        }

        if (latitude < -90.0 || latitude > 90.0) {
            send_result(command_request, false, "latitude must be in [-90,90]");
            return;
        }
        if (longitude < -180.0 || longitude > 180.0) {
            send_result(command_request, false, "longitude must be in [-180,180]");
            return;
        }
        if (altitude < static_cast<double>(std::numeric_limits<float>::lowest()) ||
            altitude > static_cast<double>(std::numeric_limits<float>::max()))
        {
            send_result(command_request, false, "altitude is outside float32 range");
            return;
        }

        if (!goto_global_client_->wait_for_service(100ms)) {
            send_result(command_request, false, "ROS service not available");
            return;
        }

        auto service_request = std::make_shared<GotoGlobal::Request>();
        service_request->latitude_deg = latitude;
        service_request->longitude_deg = longitude;
        service_request->altitude_m = static_cast<float>(altitude);
        service_request->altitude_reference =
            command_request.command == "GOTO" ?
            GotoGlobal::Request::ALTITUDE_REFERENCE_RELATIVE_HOME :
            GotoGlobal::Request::ALTITUDE_REFERENCE_AMSL;

        pending_goto_ = PendingGoto{
            make_pending_metadata(command_request),
            goto_global_client_->async_send_request(service_request),
            service_deadline()};
    }

    void poll_pending_services()
    {
        const auto now = std::chrono::steady_clock::now();

        if (pending_trigger_) {
            if (pending_trigger_->future.future.wait_for(0ms) == std::future_status::ready) {
                const PendingMetadata metadata = pending_trigger_->metadata;
                try {
                    const auto response = pending_trigger_->future.future.get();
                    send_result(metadata, response->success, response->message);
                } catch (const std::exception & exception) {
                    send_result(
                        metadata, false,
                        std::string("ROS service failed: ") + exception.what());
                }
                pending_trigger_.reset();
            } else if (now >= pending_trigger_->deadline) {
                const PendingMetadata metadata = pending_trigger_->metadata;
                const auto client_it = trigger_clients_.find(metadata.command);
                if (client_it != trigger_clients_.end()) {
                    client_it->second->remove_pending_request(pending_trigger_->future);
                }
                pending_trigger_.reset();
                send_result(metadata, false, "ROS service response timeout");
            }
        }

        if (pending_goto_) {
            if (pending_goto_->future.future.wait_for(0ms) == std::future_status::ready) {
                const PendingMetadata metadata = pending_goto_->metadata;
                try {
                    const auto response = pending_goto_->future.future.get();
                    send_result(metadata, response->success, response->message);
                } catch (const std::exception & exception) {
                    send_result(
                        metadata, false,
                        std::string("ROS service failed: ") + exception.what());
                }
                pending_goto_.reset();
            } else if (now >= pending_goto_->deadline) {
                const PendingMetadata metadata = pending_goto_->metadata;
                goto_global_client_->remove_pending_request(pending_goto_->future);
                pending_goto_.reset();
                send_result(metadata, false, "ROS service response timeout");
            }
        }
    }

    std::chrono::steady_clock::time_point service_deadline() const
    {
        return std::chrono::steady_clock::now() +
               std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(service_response_timeout_s_));
    }

    void cancel_pending_service(const std::string & reason)
    {
        if (pending_trigger_) {
            const PendingMetadata metadata = pending_trigger_->metadata;
            const auto client_it = trigger_clients_.find(metadata.command);
            if (client_it != trigger_clients_.end()) {
                client_it->second->remove_pending_request(pending_trigger_->future);
            }
            pending_trigger_.reset();
            send_result(metadata, false, reason);
        }

        if (pending_goto_) {
            const PendingMetadata metadata = pending_goto_->metadata;
            goto_global_client_->remove_pending_request(pending_goto_->future);
            pending_goto_.reset();
            send_result(metadata, false, reason);
        }
    }

    bool has_pending_service() const
    {
        return pending_trigger_.has_value() || pending_goto_.has_value();
    }

    const PendingMetadata * pending_metadata() const
    {
        if (pending_trigger_) {
            return &pending_trigger_->metadata;
        }
        if (pending_goto_) {
            return &pending_goto_->metadata;
        }
        return nullptr;
    }

    static PendingMetadata make_pending_metadata(const CommandRequest & request)
    {
        return PendingMetadata{
            request.sequence,
            request.command,
            request.payload,
            request.framed};
    }

    void send_result(
        const CommandRequest & request,
        bool success,
        const std::string & message)
    {
        send_result(make_pending_metadata(request), success, message);
    }

    void send_result(
        const PendingMetadata & metadata,
        bool success,
        const std::string & message)
    {
        const std::string frame = make_frame(
            success ? "ACK" : "ERR",
            metadata.sequence,
            metadata.command,
            message);
        write_serial(frame);

        if (metadata.framed) {
            cache_response(metadata.sequence, metadata.request_payload, frame);
        }
    }

    void cache_response(
        const std::string & sequence,
        const std::string & request_payload,
        const std::string & response_frame)
    {
        const auto existing = response_cache_.find(sequence);
        if (existing != response_cache_.end()) {
            existing->second = CachedResponse{request_payload, response_frame};
            return;
        }

        while (response_cache_order_.size() >= kDedupCacheLimit) {
            response_cache_.erase(response_cache_order_.front());
            response_cache_order_.pop_front();
        }

        response_cache_order_.push_back(sequence);
        response_cache_.emplace(
            sequence,
            CachedResponse{request_payload, response_frame});
    }

    void send_frame(
        const std::string & type,
        const std::string & sequence,
        const std::string & command,
        const std::string & message)
    {
        write_serial(make_frame(type, sequence, command, message));
    }

    static std::string make_frame(
        const std::string & type,
        const std::string & sequence,
        const std::string & command,
        const std::string & message)
    {
        const std::string payload =
            sanitize(type) + "," + sanitize(sequence) + "," +
            sanitize(command) + "," + sanitize(message);
        return "$" + payload + "*" + to_hex(xor_checksum(payload)) + "\n";
    }

    void write_serial(const std::string & text)
    {
        if (serial_fd_ < 0) {
            return;
        }

        size_t offset = 0;
        const auto deadline = std::chrono::steady_clock::now() + 50ms;

        while (offset < text.size()) {
            const ssize_t n = write(
                serial_fd_,
                text.data() + offset,
                text.size() - offset);

            if (n > 0) {
                offset += static_cast<size_t>(n);
                continue;
            }

            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
                std::chrono::steady_clock::now() < deadline)
            {
                continue;
            }

            if (n < 0) {
                RCLCPP_ERROR(this->get_logger(), "Serial write error: %s", std::strerror(errno));
            } else {
                RCLCPP_ERROR(
                    this->get_logger(),
                    "Serial short write: wrote %zu of %zu bytes",
                    offset,
                    text.size());
            }
            break;
        }
    }

    static bool parse_finite_decimal(
        const std::string & text,
        const std::string & field_name,
        double & value,
        std::string & error)
    {
        if (!is_decimal_number(text)) {
            error = field_name + " must be a finite decimal number";
            return false;
        }

        errno = 0;
        char * end = nullptr;
        value = std::strtod(text.c_str(), &end);
        if (errno == ERANGE || end == nullptr ||
            end != text.c_str() + text.size() || !std::isfinite(value))
        {
            error = field_name + " must be a finite decimal number";
            return false;
        }

        return true;
    }

    static bool is_decimal_number(const std::string & text)
    {
        if (text.empty()) {
            return false;
        }

        size_t index = 0;
        if (text[index] == '+' || text[index] == '-') {
            index++;
        }

        bool has_digit = false;
        while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index]))) {
            has_digit = true;
            index++;
        }

        if (index < text.size() && text[index] == '.') {
            index++;
            while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index]))) {
                has_digit = true;
                index++;
            }
        }

        if (!has_digit) {
            return false;
        }

        if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
            index++;
            if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
                index++;
            }

            const size_t exponent_start = index;
            while (index < text.size() &&
                std::isdigit(static_cast<unsigned char>(text[index])))
            {
                index++;
            }
            if (index == exponent_start) {
                return false;
            }
        }

        return index == text.size();
    }

    static std::string trim(const std::string & value)
    {
        const size_t first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return "";
        }
        const size_t last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    static std::string uppercase(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        return value;
    }

    static std::string first_token(const std::string & value)
    {
        const size_t end = value.find_first_of(", \t");
        return value.substr(0, end);
    }

    static bool is_flight_critical_command(const std::string & command)
    {
        return command == "ENABLE_STREAM" || command == "START_MISSION" ||
               command == "START_OFFBOARD" || command == "STOP_OFFBOARD" ||
               command == "LAND" || command == "GOTO" ||
               command == "GOTO_AMSL" || command == "RTL" || command == "ABORT";
    }

    static bool is_emergency_command(const std::string & command)
    {
        return command == "RTL" || command == "ABORT";
    }

    static bool is_printable_ascii(const std::string & value)
    {
        return std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return character >= 0x20 && character <= 0x7E;
        });
    }

    static std::string sanitize(std::string value)
    {
        for (char & c : value) {
            if (c == ',' || c == '*' || c == '$' || c == '\r' || c == '\n') {
                c = ';';
            }
        }
        return value;
    }

    static std::vector<std::string> split(const std::string & text, char delimiter)
    {
        std::vector<std::string> fields;
        size_t start = 0;

        while (true) {
            const size_t end = text.find(delimiter, start);
            if (end == std::string::npos) {
                fields.push_back(text.substr(start));
                return fields;
            }

            fields.push_back(text.substr(start, end - start));
            start = end + 1;
        }
    }

    static uint8_t xor_checksum(const std::string & payload)
    {
        uint8_t checksum = 0;
        for (unsigned char c : payload) {
            checksum ^= c;
        }
        return checksum;
    }

    static bool parse_hex_byte(const std::string & text, uint8_t & value)
    {
        if (text.size() != 2 ||
            !std::isxdigit(static_cast<unsigned char>(text[0])) ||
            !std::isxdigit(static_cast<unsigned char>(text[1])))
        {
            return false;
        }

        char * end = nullptr;
        const long parsed = std::strtol(text.c_str(), &end, 16);
        if (end == nullptr || *end != '\0' || parsed < 0 || parsed > 255) {
            return false;
        }

        value = static_cast<uint8_t>(parsed);
        return true;
    }

    static std::string to_hex(uint8_t value)
    {
        const char * digits = "0123456789ABCDEF";
        std::string out(2, '0');
        out[0] = digits[(value >> 4) & 0x0F];
        out[1] = digits[value & 0x0F];
        return out;
    }
};

int main(int argc, char * argv[])
{
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<Lr24CommandNode>());
    rclcpp::shutdown();
    return 0;
}
