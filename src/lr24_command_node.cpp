#include <rclcpp/rclcpp.hpp>
#include <std_srvs/srv/trigger.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <future>
#include <map>
#include <optional>
#include <sstream>
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

        service_names_ = {
            {"START_OFFBOARD", this->declare_parameter<std::string>("start_offboard_service", "/start_offboard")},
            {"ENABLE_STREAM", this->declare_parameter<std::string>("enable_stream_service", "/enable_offboard_stream")},
            {"START_MISSION", this->declare_parameter<std::string>("start_mission_service", "/start_mission")},
            {"STOP_OFFBOARD", this->declare_parameter<std::string>("stop_offboard_service", "/stop_offboard")},
            {"LAND", this->declare_parameter<std::string>("land_service", "/land")},
            {"STATUS", this->declare_parameter<std::string>("status_service", "/offboard_status")},
        };

        for (const auto & item : service_names_) {
            clients_[item.first] = this->create_client<std_srvs::srv::Trigger>(item.second);
        }

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
    struct PendingCommand
    {
        std::string sequence;
        std::string command;
        rclcpp::Client<std_srvs::srv::Trigger>::SharedFutureAndRequestId future;
    };

    std::string port_;
    int baud_rate_ = 115200;
    int serial_fd_ = -1;
    std::string rx_buffer_;
    rclcpp::TimerBase::SharedPtr timer_;
    std::map<std::string, std::string> service_names_;
    std::map<std::string, rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr> clients_;
    std::optional<PendingCommand> pending_;

    void timer_callback()
    {
        read_serial();
        poll_pending_service();
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
                handle_line(line);
            }
        }

        if (rx_buffer_.size() > 256) {
            rx_buffer_.clear();
            send_frame("ERR", "0", "FRAME", "RX buffer overflow");
        }
    }

    void handle_line(const std::string & line)
    {
        std::string sequence = "0";
        std::string command;

        if (!line.empty() && line.front() == '$') {
            std::string error;
            if (!parse_frame(line, sequence, command, error)) {
                send_frame("ERR", sequence, "FRAME", error);
                return;
            }
        } else {
            command = line;
        }

        command = uppercase(trim(command));
        dispatch_command(sequence, command);
    }

    bool parse_frame(
        const std::string & line,
        std::string & sequence,
        std::string & command,
        std::string & error)
    {
        size_t star = line.find('*');
        if (star == std::string::npos || star + 2 >= line.size()) {
            error = "missing checksum";
            return false;
        }

        std::string payload = line.substr(1, star - 1);
        std::string checksum_text = line.substr(star + 1, 2);
        uint8_t expected = 0;
        if (!parse_hex_byte(checksum_text, expected)) {
            error = "invalid checksum text";
            return false;
        }

        uint8_t actual = xor_checksum(payload);
        if (actual != expected) {
            std::ostringstream oss;
            oss << "bad checksum expected " << to_hex(actual);
            error = oss.str();
            return false;
        }

        std::vector<std::string> fields = split(payload, ',');
        if (fields.size() < 3 || fields[0] != "CMD") {
            error = "expected CMD,seq,command";
            return false;
        }

        sequence = fields[1];
        command = fields[2];
        return true;
    }

    void dispatch_command(const std::string & sequence, const std::string & command)
    {
        if (pending_) {
            send_frame("ERR", sequence, command, "busy waiting for previous ROS service");
            return;
        }

        if (command == "PING") {
            send_frame("ACK", sequence, command, "PONG");
            return;
        }

        if (command == "HELP") {
            send_frame("ACK", sequence, command, "PING ENABLE_STREAM START_MISSION START_OFFBOARD STOP_OFFBOARD LAND STATUS");
            return;
        }

        auto client_it = clients_.find(command);
        if (client_it == clients_.end()) {
            send_frame("ERR", sequence, command, "unknown command");
            return;
        }

        auto client = client_it->second;
        if (!client->wait_for_service(100ms)) {
            send_frame("ERR", sequence, command, "ROS service not available");
            return;
        }

        auto request = std::make_shared<std_srvs::srv::Trigger::Request>();
        pending_ = PendingCommand{
            sequence,
            command,
            client->async_send_request(request)};
    }

    void poll_pending_service()
    {
        if (!pending_) {
            return;
        }

        if (pending_->future.future.wait_for(0ms) != std::future_status::ready) {
            return;
        }

        auto response = pending_->future.future.get();
        send_frame(
            response->success ? "ACK" : "ERR",
            pending_->sequence,
            pending_->command,
            response->message);
        pending_.reset();
    }

    void send_frame(
        const std::string & type,
        const std::string & sequence,
        const std::string & command,
        const std::string & message)
    {
        std::string payload =
            type + "," + sanitize(sequence) + "," + sanitize(command) + "," + sanitize(message);
        std::string frame = "$" + payload + "*" + to_hex(xor_checksum(payload)) + "\n";
        write_serial(frame);
    }

    void write_serial(const std::string & text)
    {
        if (serial_fd_ < 0) {
            return;
        }

        ssize_t written = write(serial_fd_, text.data(), text.size());
        if (written < 0) {
            RCLCPP_ERROR(this->get_logger(), "Serial write error: %s", std::strerror(errno));
        }
    }

    static std::string trim(const std::string & value)
    {
        size_t first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return "";
        }
        size_t last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    static std::string uppercase(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        return value;
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
        std::stringstream ss(text);
        std::string item;
        while (std::getline(ss, item, delimiter)) {
            fields.push_back(item);
        }
        return fields;
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
        if (text.size() != 2) {
            return false;
        }

        char * end = nullptr;
        long parsed = std::strtol(text.c_str(), &end, 16);
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
