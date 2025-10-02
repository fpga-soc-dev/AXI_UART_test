#include "UartDevice.hpp"

#include <fcntl.h>
#include <stdexcept>
#include <vector>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <cmath>
#include <fmt/core.h>
#include <fmt/format.h>

namespace UART
{
    speed_t BaudRateToSpeed(BaudRate baud_rate)
    {
        switch (baud_rate)
        {
        case BaudRate::BAUD_9600:
            return B9600;
        case BaudRate::BAUD_19200:
            return B19200;
        case BaudRate::BAUD_38400:
            return B38400;
        case BaudRate::BAUD_57600:
            return B57600;
        case BaudRate::BAUD_115200:
            return B115200;
        case BaudRate::BAUD_230400:
            return B230400;
        default:
            return B115200;
        }
    }

    // O_NOCTTY: This flag tells the system not to make the opened file (in this case, the UART device) the controlling terminal for the process.
    // Without this flag, if your process opens a terminal device (like a UART), it could become the controlling terminal, which can lead to unexpected behavior (like receiving signals such as SIGINT from the terminal).
    UartDevice::UartDevice(const std::string &device_path)
        : m_DevicePath(device_path),
          m_FileHandle(std::make_unique<Utils::FileHandle>(device_path, (O_RDWR | O_NOCTTY)))
    {
        if (!m_FileHandle->IsOpen())
        {
            throw std::runtime_error("Failed to open UART device: " + device_path);
        }
        ApplyConfig();
    }

    UartDevice::UartDevice(const std::string &device_path, const UartConfig &config)
        : m_DevicePath(device_path),
          m_Config(config),
          m_FileHandle(std::make_unique<Utils::FileHandle>(device_path, (O_RDWR | O_NOCTTY)))
    {
        if (!m_FileHandle->IsOpen())
        {
            throw std::runtime_error("Failed to open UART device: " + device_path);
        }
        ApplyConfig();
    }

    UartDevice::~UartDevice()
    {
        DisableInterrupts();
    }

    UartDevice::UartDevice(UartDevice &&other) noexcept
        : m_FileHandle(std::move(other.m_FileHandle)),
          m_DevicePath(std::move(other.m_DevicePath)),
          m_Config(other.m_Config),
          m_InterruptThread(std::move(other.m_InterruptThread)),
          m_InterruptEnabled(other.m_InterruptEnabled.load()),
          m_InterruptCallback(std::move(other.m_InterruptCallback))
    {
        other.m_InterruptEnabled.store(false);
    }

    UartDevice &UartDevice::operator=(UartDevice &&other) noexcept
    {
        if (this != &other)
        {
            DisableInterrupts();

            m_FileHandle = std::move(other.m_FileHandle);
            m_DevicePath = std::move(other.m_DevicePath);
            m_Config = other.m_Config;
            m_InterruptThread = std::move(other.m_InterruptThread);
            m_InterruptEnabled.store(other.m_InterruptEnabled.load());
            m_InterruptCallback = std::move(other.m_InterruptCallback);

            other.m_InterruptEnabled.store(false);
        }
        return *this;
    }

    void UartDevice::Configure(const UartConfig &config)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Config = config;
        ApplyConfig();
    }

    UartConfig UartDevice::GetConfig() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Config;
    }

    bool UartDevice::IsOpen() const
    {
        return m_FileHandle && m_FileHandle->IsOpen();
    }

    size_t UartDevice::Write(std::span<const uint8_t> data)
    {
        if (!IsOpen())
        {
            throw std::runtime_error("UART device not open");
        }

        if (data.empty())
        {
            return 0;
        }

        size_t total_written = 0;
        constexpr size_t MAX_CHUNK_SIZE = 16; // AXI UART FIFO size

        while (total_written < data.size())
        {
            size_t remaining = data.size() - total_written;
            size_t chunk_size = std::min(remaining, MAX_CHUNK_SIZE);

            ssize_t bytes_written = m_FileHandle->Write(data.data() + total_written, chunk_size);

            if (bytes_written > 0)
            {
                total_written += static_cast<size_t>(bytes_written);
            }
            else if (bytes_written == 0)
            {
                throw std::runtime_error("UART write returned 0 bytes");
            }
            else
            {
                if (errno == EINTR)
                {
                    continue;
                }
                else if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                else
                {
                    throw std::runtime_error("UART write failed: " + std::string(strerror(errno)));
                }
            }
        }

        return total_written;
    }

    std::vector<uint8_t> UartDevice::Read(int timeout_ms)
    {
        std::vector<uint8_t> data;
        constexpr size_t CHUNK_SIZE = 4096;

        struct pollfd pfd = {
            .fd = m_FileHandle->GetFd(),
            .events = POLLIN,
            .revents = 0};

        while (true)
        {
            int poll_result = ::poll(&pfd, 1, timeout_ms);

            if (poll_result < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                throw std::runtime_error("Poll failed: " + std::string(strerror(errno)));
            }

            if (poll_result == 0)
            {
                // Timeout - if we have data without delimiter, return it anyway
                if (!data.empty())
                {
                    break;
                }
                continue;
            }

            if (pfd.revents & POLLIN)
            {
                size_t bytes_available = BytesAvailable();
                if (bytes_available > 0)
                {
                    size_t to_read = std::min(bytes_available, CHUNK_SIZE);
                    std::vector<uint8_t> buffer(to_read);

                    // Block until we read some data during timeout period
                    ssize_t bytes_read = m_FileHandle->Read(buffer.data(), buffer.size());

                    if (bytes_read > 0)
                    {
                        buffer.resize(static_cast<size_t>(bytes_read));
                        data.insert(data.end(), buffer.begin(), buffer.end());

                        if (std::find(buffer.begin(), buffer.end(), MESSAGE_DELIMITER) != buffer.end())
                        {
                            break;
                        }
                    }
                    else if (bytes_read < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                        {
                            continue;
                        }
                        throw std::runtime_error("Read failed: " + std::string(strerror(errno)));
                    }
                    else
                    {
                        break;
                    }
                }
            }

            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            {
                break;
            }

            pfd.revents = 0;
        }

        return data;
    }

    void UartDevice::RegisterDataCallback(const InterruptCallback &callback)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        if (m_InterruptEnabled.load())
        {
            throw std::runtime_error("Interrupts already enabled");
        }

        if (m_InterruptCallback)
        {
            throw std::runtime_error("Callback already registered");
        }

        m_InterruptCallback = callback;
        m_InterruptEnabled.store(true);

        if (!m_InterruptThread.joinable())
        {
            m_InterruptThread = std::jthread([this](std::stop_token token)
                                             { InterruptHandler(token); });
        }
    }

    void UartDevice::DisableInterrupts()
    {
        m_InterruptEnabled.store(false);
        if (m_InterruptThread.joinable())
        {
            m_InterruptThread.request_stop();
            m_InterruptThread.join();
        }
    }

    void UartDevice::Flush()
    {
        if (!IsOpen())
        {
            throw std::runtime_error("UART device not open");
        }
        tcflush(m_FileHandle->GetFd(), TCIOFLUSH); // Flush both buffers (
    }

    size_t UartDevice::BytesAvailable() const
    {
        if (!IsOpen())
        {
            throw std::runtime_error("UART device not open");
        }

        // Use ioctl to get the number of bytes available for reading
        int bytes_available = 0;
        if (ioctl(m_FileHandle->GetFd(), FIONREAD, &bytes_available) == 0)
        {
            return static_cast<size_t>(bytes_available);
        }
        return 0;
    }

    void UartDevice::ApplyConfig()
    {
        if (m_FileHandle->GetFd() < 0)
        {
            throw std::runtime_error("UART device not open");
        }

        // Get current terminal attributes before modification
        struct termios tty;
        if (tcgetattr(m_FileHandle->GetFd(), &tty) != 0)
        {
            throw std::runtime_error("Failed to get UART attributes");
        }

        // Both TX and RX are configured for m_Config.baud_rate bits per second
        speed_t speed = BaudRateToSpeed(m_Config.baud_rate);
        cfsetospeed(&tty, speed);
        cfsetispeed(&tty, speed);

        // Configure data bits
        tty.c_cflag &= ~CSIZE;
        switch (m_Config.data_bits)
        {
        case DataBits::DATA_5:
            tty.c_cflag |= CS5;
            break;
        case DataBits::DATA_6:
            tty.c_cflag |= CS6;
            break;
        case DataBits::DATA_7:
            tty.c_cflag |= CS7;
            break;
        case DataBits::DATA_8:
            tty.c_cflag |= CS8;
            break;
        default:
            tty.c_cflag |= CS8;
            break;
        }
        // Configure parity
        switch (m_Config.parity)
        {
        case Parity::NONE:
            tty.c_cflag &= ~PARENB;
            break;
        case Parity::ODD:
            tty.c_cflag |= PARENB;
            tty.c_cflag |= PARODD;
            break;
        case Parity::EVEN:
            tty.c_cflag |= PARENB;
            tty.c_cflag &= ~PARODD;
            break;
        default:
            tty.c_cflag &= ~PARENB;
            break;
        }

        tty.c_cflag &= ~CSTOPB;                                                      // Set 1 stop bit (clear CSTOPB flag)
        tty.c_cflag |= (CLOCAL | CREAD);                                             // Enable local mode and receiver
        tty.c_cflag &= ~CRTSCTS;                                                     // Disable hardware flow control (RTS/CTS)
        tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);                              // Disable line processing features
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);                                      // Disable software flow control (XON/XOFF)
        tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL); // Disable input processing
        tty.c_oflag &= ~OPOST;                                                       // Disable output processing
        tty.c_cc[VTIME] = 0;                                                         // Set read timeout (deciseconds)
        tty.c_cc[VMIN] = 0;                                                          // Set minimum characters for read (0 = non-blocking with timeout)

        if (tcsetattr(m_FileHandle->GetFd(), TCSANOW, &tty) != 0)
        {
            throw std::runtime_error("Failed to set terminal attributes");
        }

        PrintTermiosSettings(tty);
    }

    void UartDevice::InterruptHandler(std::stop_token token)
    {
        int epoll_fd = epoll_create1(0); // create epoll instance for monitoring UART fd
        if (epoll_fd == -1)
        {
            throw std::runtime_error("Failed to create epoll instance");
        }

        struct epoll_event event;
        event.events = EPOLLIN;                // Monitor for read availability
        event.data.fd = m_FileHandle->GetFd(); // Store the UART fd in event data

        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, m_FileHandle->GetFd(), &event) == -1)
        {
            close(epoll_fd);
            throw std::runtime_error("Failed to add UART fd to epoll");
        }

        struct epoll_event events[1];
        std::vector<uint8_t> accumulated_data;

        while (!token.stop_requested() && m_InterruptEnabled.load())
        {
            // Wait for data without timeout - we rely on delimiter for message boundaries
            int nfds = epoll_wait(epoll_fd, events, 1, 100); // Short timeout for responsiveness

            if (nfds == -1)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                break; // Error occurred
            }

            if (nfds > 0 && (events[0].events & EPOLLIN))
            {
                try
                {
                    // Read all immediately available data
                    auto new_data = Read();
                    if (!new_data.empty())
                    {
                        accumulated_data.insert(accumulated_data.end(), new_data.begin(), new_data.end());

                        // Check if we have complete message(s) with delimiter
                        auto delimiter_pos = std::find(accumulated_data.begin(), accumulated_data.end(), MESSAGE_DELIMITER);

                        while (delimiter_pos != accumulated_data.end())
                        {
                            // Extract complete message (including delimiter)
                            size_t message_length = std::distance(accumulated_data.begin(), delimiter_pos) + 1;
                            std::vector<uint8_t> complete_message(accumulated_data.begin(), accumulated_data.begin() + message_length);

                            // Send the complete message via callback
                            if (m_InterruptCallback)
                            {
                                InterruptEvent interrupt_event{complete_message.size(), std::move(complete_message)};
                                m_InterruptCallback(interrupt_event);
                            }

                            // Remove processed message from buffer
                            accumulated_data.erase(accumulated_data.begin(), accumulated_data.begin() + message_length);

                            // Check for more complete messages
                            delimiter_pos = std::find(accumulated_data.begin(), accumulated_data.end(), MESSAGE_DELIMITER);
                        }
                    }
                }
                catch (const std::exception &e)
                {
                    // Handle errors - send empty event to signal error
                    if (m_InterruptCallback)
                    {
                        InterruptEvent interrupt_event{0, {}};
                        m_InterruptCallback(interrupt_event);
                    }
                    accumulated_data.clear();
                    continue;
                }
            }

            // Handle epoll errors
            if (nfds > 0 && (events[0].events & (EPOLLERR | EPOLLHUP)))
            {
                break; // Connection error
            }
        }

        close(epoll_fd);
    }

    void UartDevice::PrintTermiosSettings(const struct termios &tty) const
    {
        fmt::print("---UART Settings---\n");

        // Baud rate
        speed_t ispeed = cfgetispeed(&tty);
        speed_t ospeed = cfgetospeed(&tty);
        fmt::print("Input Baud: {}, Output Baud: {}\n", SpeedToBaudRate(ispeed), SpeedToBaudRate(ospeed));

        // Control flags
        fmt::print("c_cflag: 0x{:08X}\n", tty.c_cflag);
        fmt::print("  Data bits: {}\n", (tty.c_cflag & CSIZE) == CS5 ? "5" : (tty.c_cflag & CSIZE) == CS6 ? "6"
                                                                         : (tty.c_cflag & CSIZE) == CS7   ? "7"
                                                                         : (tty.c_cflag & CSIZE) == CS8   ? "8"
                                                                                                          : "Unknown");
        fmt::print("  Parity: {}\n", (tty.c_cflag & PARENB) ? ((tty.c_cflag & PARODD) ? "ODD" : "EVEN") : "NONE");
        fmt::print("  Stop bits: {}\n", (tty.c_cflag & CSTOPB) ? "2" : "1");
        fmt::print("  Flow control: {}\n", (tty.c_cflag & CRTSCTS) ? "RTS/CTS" : "NONE");
        fmt::print("  Local/Read: CLOCAL={}, CREAD={}\n",
                   (tty.c_cflag & CLOCAL) ? "ON" : "OFF",
                   (tty.c_cflag & CREAD) ? "ON" : "OFF");

        // Local flags
        fmt::print("c_lflag: 0x{:08X}\n", tty.c_lflag);
        fmt::print("  Canonical: {}\n", (tty.c_lflag & ICANON) ? "ON" : "OFF");
        fmt::print("  Echo: {}\n", (tty.c_lflag & ECHO) ? "ON" : "OFF");
        fmt::print("  Signals: {}\n", (tty.c_lflag & ISIG) ? "ON" : "OFF");

        // Input flags
        fmt::print("c_iflag: 0x{:08X}\n", tty.c_iflag);
        fmt::print("  XON/XOFF: {}\n", (tty.c_iflag & (IXON | IXOFF)) ? "ON" : "OFF");
        fmt::print("  CR/LF conversion: INLCR={}, IGNCR={}, ICRNL={}\n",
                   (tty.c_iflag & INLCR) ? "ON" : "OFF",
                   (tty.c_iflag & IGNCR) ? "ON" : "OFF",
                   (tty.c_iflag & ICRNL) ? "ON" : "OFF");

        // Output flags
        fmt::print("c_oflag: 0x{:08X}\n", tty.c_oflag);
        fmt::print("  Output processing: {}\n", (tty.c_oflag & OPOST) ? "ON" : "OFF");

        // Control characters
        fmt::print("VMIN: {}, VTIME: {} ({}ms)\n",
                   tty.c_cc[VMIN], tty.c_cc[VTIME], tty.c_cc[VTIME] * 100);

        fmt::print("\n");
    }

    std::string UartDevice::SpeedToBaudRate(speed_t speed) const
    {
        switch (speed)
        {
        case B9600:
            return "9600";
        case B19200:
            return "19200";
        case B38400:
            return "38400";
        case B57600:
            return "57600";
        case B115200:
            return "115200";
        case B230400:
            return "230400";
        default:
            return fmt::format("Unknown({})", speed);
        }
    }

} // namespace UART