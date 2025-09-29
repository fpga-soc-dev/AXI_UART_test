#include "UartDevice.hpp"

#include <fcntl.h>
#include <stdexcept>
#include <termios.h>
#include <vector>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <cmath>

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
    UartDevice::UartDevice(const std::string &device_path, UartMode mode)
        : m_DevicePath(device_path),
          m_Mode(mode),
          m_FileHandle(std::make_unique<Utils::FileHandle>(device_path, O_RDWR | O_NOCTTY | (mode == UartMode::NON_BLOCKING ? O_NONBLOCK : 0)))
    {

        if (!m_FileHandle->IsOpen())
        {
            throw std::runtime_error("Failed to open UART device: " + device_path);
        }
        ApplyConfig();
    }

    UartDevice::UartDevice(const std::string &device_path, const UartConfig &config, UartMode mode)
        : m_DevicePath(device_path),
          m_Config(config),
          m_Mode(mode),
          m_FileHandle(std::make_unique<Utils::FileHandle>(device_path, O_RDWR | O_NOCTTY | (mode == UartMode::NON_BLOCKING ? O_NONBLOCK : 0)))
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

        if (m_Mode == UartMode::BLOCKING)
        {
            return WriteBlocking(data);
        }
        else
        {
            return WriteNonBlocking(data);
        }
    }

    size_t UartDevice::WriteBlocking(std::span<const uint8_t> data)
    {
        if (data.empty())
        {
            return 0;
        }

        size_t total_written = 0;
        constexpr size_t MAX_CHUNK_SIZE = 512;
        auto start_time = std::chrono::steady_clock::now();

        // Replace the lambda with mathematical calculation
        auto write_timeout = CalculateAdaptiveWriteTimeout();

        struct pollfd pfd = {
            .fd = m_FileHandle->GetFd(),
            .events = POLLOUT,
            .revents = 0};

        while (total_written < data.size())
        {
            auto current_time = std::chrono::steady_clock::now();

            if (current_time - start_time > write_timeout)
            {
                throw std::runtime_error("UART write timeout after " +
                                         std::to_string(total_written) + " bytes written");
            }

            size_t remaining = data.size() - total_written;
            size_t chunk_size = std::min(remaining, MAX_CHUNK_SIZE);

            ssize_t bytes_written = m_FileHandle->Write(data.data() + total_written, chunk_size);

            if (bytes_written > 0)
            {
                total_written += static_cast<size_t>(bytes_written);

                if (total_written < data.size())
                {
                    // Mathematical delay calculation
                    uint32_t baud = static_cast<uint32_t>(m_Config.baud_rate);
                    constexpr uint32_t THRESHOLD_BAUD = 115200;

                    // Linear interpolation between 50μs and 200μs based on baud rate
                    double delay_factor = std::max(0.0, 1.0 - (static_cast<double>(baud) / THRESHOLD_BAUD));
                    int delay_us = static_cast<int>(50 + delay_factor * 150); // 50-200μs range

                    std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
                }

                pfd.revents = 0;
                continue;
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
                    auto remaining_timeout = write_timeout - (current_time - start_time);
                    int poll_timeout_ms = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(remaining_timeout).count());

                    // Mathematical poll timeout calculation
                    uint32_t baud = static_cast<uint32_t>(m_Config.baud_rate);
                    constexpr uint32_t FAST_BAUD_THRESHOLD = 115200;

                    // Linear scaling: 100ms for fast, 250ms for slow
                    int max_poll_timeout = static_cast<int>(
                        100 + (FAST_BAUD_THRESHOLD >= baud ? 150.0 * (FAST_BAUD_THRESHOLD - baud) / FAST_BAUD_THRESHOLD : 0));

                    poll_timeout_ms = std::min(poll_timeout_ms, max_poll_timeout);

                    if (poll_timeout_ms <= 0)
                    {
                        throw std::runtime_error("UART write timeout waiting for buffer space");
                    }

                    pfd.revents = 0;
                    int poll_result = ::poll(&pfd, 1, poll_timeout_ms);

                    if (poll_result < 0)
                    {
                        if (errno == EINTR)
                        {
                            continue;
                        }
                        throw std::runtime_error("Poll failed during write: " + std::string(strerror(errno)));
                    }
                    else if (poll_result == 0)
                    {
                        continue;
                    }
                    else if (pfd.revents & POLLOUT)
                    {
                        continue;
                    }
                    else if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
                    {
                        throw std::runtime_error("UART write error detected by poll");
                    }
                }
                else
                {
                    throw std::runtime_error("UART write failed: " + std::string(strerror(errno)));
                }
            }
        }

        return total_written;
    }

    size_t UartDevice::WriteNonBlocking(std::span<const uint8_t> data)
    {
        if (data.empty())
        {
            return 0;
        }

        size_t total_written = 0;
        constexpr size_t MAX_CHUNK_SIZE = 512;
        auto start_time = std::chrono::steady_clock::now();
        auto write_timeout = CalculateAdaptiveWriteTimeout();

        const int fd = m_FileHandle->GetFd();
        struct pollfd pfd = {
            .fd = fd,
            .events = POLLOUT,
            .revents = 0};

        while (total_written < data.size())
        {
            auto current_time = std::chrono::steady_clock::now();

            // Check timeout
            if (current_time - start_time > write_timeout)
            {
                return total_written; // Return partial write instead of throwing
            }

            size_t remaining = data.size() - total_written;
            size_t chunk_size = std::min(remaining, MAX_CHUNK_SIZE);

            try
            {
                ssize_t bytes_written = m_FileHandle->Write(data.data() + total_written, chunk_size);

                if (bytes_written > 0)
                {
                    total_written += static_cast<size_t>(bytes_written);

                    // Brief adaptive delay for flow control
                    uint32_t baud = static_cast<uint32_t>(m_Config.baud_rate);
                    int delay_us = (baud >= 115200) ? 10 : 50; // Shorter delays for non-blocking
                    std::this_thread::sleep_for(std::chrono::microseconds(delay_us));

                    continue;
                }
                else if (bytes_written == 0)
                {
                    // This shouldn't happen, but handle gracefully
                    break;
                }
            }
            catch (const std::system_error &e)
            {
                if (e.code().value() == EAGAIN || e.code().value() == EWOULDBLOCK)
                {
                    // Write buffer full, use poll to wait for space
                    auto remaining_timeout = write_timeout - (current_time - start_time);
                    int poll_timeout_ms = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(remaining_timeout).count());

                    // Adaptive poll timeout
                    uint32_t baud = static_cast<uint32_t>(m_Config.baud_rate);
                    int max_poll_timeout = (baud >= 115200) ? 50 : 100; // Shorter for non-blocking
                    poll_timeout_ms = std::min(poll_timeout_ms, max_poll_timeout);

                    if (poll_timeout_ms <= 0)
                    {
                        break; // Timeout, return partial write
                    }

                    pfd.revents = 0;
                    int poll_result = ::poll(&pfd, 1, poll_timeout_ms);

                    if (poll_result <= 0)
                    {
                        continue; // Timeout or error, check main timeout on next iteration
                    }
                    else if (pfd.revents & POLLOUT)
                    {
                        continue; // Buffer has space, try writing again
                    }
                    else if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
                    {
                        break; // Error condition
                    }
                }
                else
                {
                    throw; // Re-throw other errors
                }
            }
        }

        return total_written;
    }

    std::vector<uint8_t> UartDevice::Read()
    {
        if (!IsOpen())
        {
            throw std::runtime_error("UART device not open");
        }

        if (m_Mode == UartMode::NON_BLOCKING)
        {
            return ReadNonBlocking();
        }
        else
        {
            return ReadBlocking();
        }
    }

    std::vector<uint8_t> UartDevice::ReadBlocking()
    {
        std::vector<uint8_t> data;
        constexpr size_t CHUNK_SIZE = 4096;
        auto start_time = std::chrono::steady_clock::now();
        auto last_data_time = start_time;

        auto [idle_timeout, inter_chunk_timeout, total_timeout_multiplier] = CalculateAdaptiveReadTimeouts();

        struct pollfd pfd = {
            .fd = m_FileHandle->GetFd(),
            .events = POLLIN,
            .revents = 0};

        while (true)
        {
            auto current_time = std::chrono::steady_clock::now();
            auto adaptive_total_timeout = m_Config.read_timeout * total_timeout_multiplier;
            if (current_time - start_time > adaptive_total_timeout)
            {
                break;
            }

            if (current_time - last_data_time > idle_timeout && !data.empty())
            {
                break;
            }

            auto remaining_total = std::chrono::duration_cast<std::chrono::milliseconds>(
                adaptive_total_timeout - (current_time - start_time));
            auto remaining_idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                idle_timeout - (current_time - last_data_time));

            auto poll_timeout = std::min(remaining_total, remaining_idle);

            int poll_timeout_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(poll_timeout).count());

            poll_timeout_ms = std::max(poll_timeout_ms, 10);

            int poll_result = ::poll(&pfd, 1, poll_timeout_ms);

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
                if (data.empty())
                {
                    continue;
                }
                else
                {
                    break;
                }
            }

            if (pfd.revents & POLLIN)
            {
                size_t bytes_available = BytesAvailable();
                if (bytes_available > 0)
                {
                    size_t to_read = std::min(bytes_available, CHUNK_SIZE);
                    std::vector<uint8_t> buffer(to_read);

                    ssize_t bytes_read = m_FileHandle->Read(buffer.data(), buffer.size());

                    if (bytes_read > 0)
                    {
                        buffer.resize(static_cast<size_t>(bytes_read));
                        data.insert(data.end(), buffer.begin(), buffer.end());
                        last_data_time = current_time;
                    }
                    else if (bytes_read < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                        {
                            continue;
                        }
                        else if (errno == EINTR)
                        {
                            continue;
                        }
                        else
                        {
                            throw std::runtime_error("Read failed: " + std::string(strerror(errno)));
                        }
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

    std::vector<uint8_t> UartDevice::ReadNonBlocking()
    {
        std::vector<uint8_t> data;
        constexpr size_t CHUNK_SIZE = 4096;

        // In non-blocking mode, we read all immediately available data and return
        while (true)
        {
            // Check if data is available without blocking
            size_t bytes_available = BytesAvailable();
            if (bytes_available == 0)
            {
                break; // No data available, return what we have
            }

            // Read available data in chunks
            size_t to_read = std::min(bytes_available, CHUNK_SIZE);
            std::vector<uint8_t> buffer(to_read);

            try
            {
                ssize_t bytes_read = m_FileHandle->Read(buffer.data(), buffer.size());

                if (bytes_read > 0)
                {
                    buffer.resize(static_cast<size_t>(bytes_read));
                    data.insert(data.end(), buffer.begin(), buffer.end());

                    // If we read less than available, there might be more
                    // Continue the loop to get the rest
                    if (static_cast<size_t>(bytes_read) < bytes_available)
                    {
                        continue;
                    }
                }
                else if (bytes_read == 0)
                {
                    break; // EOF
                }
                else if (bytes_read < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        // No data available right now (expected in non-blocking mode)
                        break;
                    }
                    else if (errno == EINTR)
                    {
                        continue; // Interrupted by signal, try again
                    }
                    else
                    {
                        throw std::runtime_error("Non-blocking read failed: " + std::string(strerror(errno)));
                    }
                }
            }
            catch (const std::system_error &e)
            {
                // Handle FileHandle throwing system_error for EAGAIN
                if (e.code().value() == EAGAIN || e.code().value() == EWOULDBLOCK)
                {
                    break; // No data available, return what we have
                }
                throw; // Re-throw other errors
            }
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
        tcflush(m_FileHandle->GetFd(), TCIOFLUSH);
    }

    size_t UartDevice::BytesAvailable() const
    {
        if (!IsOpen())
        {
            throw std::runtime_error("UART device not open");
        }

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

        struct termios tty;
        if (tcgetattr(m_FileHandle->GetFd(), &tty) != 0)
        {
            throw std::runtime_error("Failed to get UART attributes");
        }

        speed_t speed = BaudRateToSpeed(m_Config.baud_rate);
        cfsetospeed(&tty, speed);
        cfsetispeed(&tty, speed);

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

        // Set stop bits (default is 1, set CSTOPB for 2)
        tty.c_cflag &= ~CSTOPB;

        tty.c_cflag |= (CLOCAL | CREAD);
        tty.c_cflag &= ~CRTSCTS;

        tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);

        tty.c_oflag &= ~OPOST;

        tty.c_cc[VTIME] = static_cast<cc_t>(m_Config.read_timeout.count() / 100);
        tty.c_cc[VMIN] = 0;

        if (tcsetattr(m_FileHandle->GetFd(), TCSANOW, &tty) != 0)
        {
            throw std::runtime_error("Failed to set terminal attributes");
        }
    }

    void UartDevice::InterruptHandler(std::stop_token token)
    {
        int epoll_fd = epoll_create1(0);
        if (epoll_fd == -1)
        {
            throw std::runtime_error("Failed to create epoll instance");
        }

        struct epoll_event event;
        event.events = EPOLLIN;
        event.data.fd = m_FileHandle->GetFd();

        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, m_FileHandle->GetFd(), &event) == -1)
        {
            close(epoll_fd);
            throw std::runtime_error("Failed to add UART fd to epoll");
        }

        struct epoll_event events[1];
        std::vector<uint8_t> accumulated_data;

        auto last_data_time = std::chrono::steady_clock::now();
        auto [idle_timeout, inter_chunk_timeout, total_timeout_multiplier] = CalculateAdaptiveReadTimeouts();

        while (!token.stop_requested() && m_InterruptEnabled.load())
        {
            int epoll_timeout_ms = static_cast<int>(inter_chunk_timeout.count());
            int nfds = epoll_wait(epoll_fd, events, 1, epoll_timeout_ms);

            if (nfds == -1)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                break;
            }

            auto current_time = std::chrono::steady_clock::now();

            if (nfds > 0 && (events[0].events & EPOLLIN))
            {
                try
                {
                    // Use non-blocking read - gets all immediately available data
                    auto new_data = ReadNonBlocking();
                    if (!new_data.empty())
                    {
                        accumulated_data.insert(accumulated_data.end(), new_data.begin(), new_data.end());
                        last_data_time = current_time;
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

            // Check if we should send accumulated data (adaptive idle timeout)
            if (!accumulated_data.empty() &&
                (current_time - last_data_time) > idle_timeout &&
                m_InterruptCallback)
            {
                InterruptEvent interrupt_event{accumulated_data.size(), std::move(accumulated_data)};
                m_InterruptCallback(interrupt_event);
                accumulated_data.clear();
                last_data_time = current_time;
            }
        }

        close(epoll_fd);
    }

    std::tuple<std::chrono::milliseconds, std::chrono::milliseconds, int>
    UartDevice::CalculateAdaptiveReadTimeouts() const
    {
        uint32_t baud = static_cast<uint32_t>(m_Config.baud_rate);
        constexpr uint32_t REFERENCE_BAUD = 230400;

        double baud_ratio = static_cast<double>(REFERENCE_BAUD) / baud;
        double sqrt_scaling = std::sqrt(baud_ratio);

        constexpr int BASE_IDLE_MS = 100;
        constexpr int BASE_INTER_CHUNK_MS = 5;
        constexpr int BASE_MULTIPLIER = 1;

        auto idle_timeout = std::chrono::milliseconds(
            static_cast<int>(std::clamp(BASE_IDLE_MS * sqrt_scaling, 50.0, 1000.0)));

        auto inter_chunk_timeout = std::chrono::milliseconds(
            static_cast<int>(std::clamp(BASE_INTER_CHUNK_MS * baud_ratio, 5.0, 100.0)));

        int total_timeout_multiplier = static_cast<int>(
            std::clamp(BASE_MULTIPLIER * std::min(sqrt_scaling, 10.0), 1.0, 10.0));

        return {idle_timeout, inter_chunk_timeout, total_timeout_multiplier};
    }

    std::chrono::milliseconds UartDevice::CalculateAdaptiveWriteTimeout() const
    {
        uint32_t baud = static_cast<uint32_t>(m_Config.baud_rate);
        constexpr uint32_t REFERENCE_BAUD = 230400;

        double baud_ratio = static_cast<double>(REFERENCE_BAUD) / baud;
        double sqrt_scaling = std::sqrt(baud_ratio);

        auto scaled_timeout = std::chrono::milliseconds(
            static_cast<int>(m_Config.write_timeout.count() * std::clamp(sqrt_scaling, 1.0, 10.0)));

        return scaled_timeout;
    }
} // namespace UART