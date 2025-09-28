#include "UartDevice.hpp"

#include <fcntl.h>
#include <stdexcept>
#include <termios.h>
#include <vector>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>

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
          m_FileHandle(std::make_unique<Utils::FileHandle>(device_path, O_RDWR | O_NOCTTY))
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
          m_FileHandle(std::make_unique<Utils::FileHandle>(device_path, O_RDWR | O_NOCTTY))

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
        ssize_t bytes_written = m_FileHandle->Write(data.data(), data.size());
        if (bytes_written < 0)
        {
            throw std::runtime_error("Failed to write to UART device");
        }
        return static_cast<size_t>(bytes_written);
    }

    std::vector<uint8_t> UartDevice::Read()
    {
        if (!IsOpen())
        {
            throw std::runtime_error("UART device not open");
        }

        size_t available_bytes = BytesAvailable();
        if (available_bytes == 0)
        {
            return {};
        }
        std::vector<uint8_t> data;
        data.reserve(available_bytes);

        constexpr size_t CHUNK_SIZE = 256;
        std::array<uint8_t, CHUNK_SIZE> temp_buffer;

        while (true)
        {
            ssize_t bytes_read = m_FileHandle->Read(temp_buffer.data(), temp_buffer.size());

            if (bytes_read < 0)
            {
                throw std::runtime_error("Failed to read from UART device");
            }

            if (bytes_read == 0)
            {
                break;
            }

            data.insert(data.end(), temp_buffer.begin(), temp_buffer.begin() + bytes_read);

            if (static_cast<size_t>(bytes_read) < CHUNK_SIZE)
            {
                break;
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

        while (!token.stop_requested() && m_InterruptEnabled.load())
        {
            int nfds = epoll_wait(epoll_fd, events, 1, 100); // 100 ms timeout

            if (nfds == -1)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                break;
            }

            if (nfds > 0 && (events[0].events & EPOLLIN))
            {
                try
                {
                    // Read all available data
                    auto new_data = Read();
                    if (!new_data.empty())
                    {
                        accumulated_data.insert(accumulated_data.end(), new_data.begin(), new_data.end());
                    }

                    // Wait a short time to see if more data arrives (debouncing)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));

                    // Check if more data is available
                    if (BytesAvailable() == 0 && !accumulated_data.empty() && m_InterruptCallback)
                    {
                        // No more data available, send the accumulated data
                        InterruptEvent interrupt_event{accumulated_data.size(), std::move(accumulated_data)};
                        m_InterruptCallback(interrupt_event);
                        accumulated_data.clear(); // Reset for next batch
                    }
                }
                catch (const std::exception &e)
                {
                    // Handle read errors gracefully
                    InterruptEvent interrupt_event{0, {}};
                    if (m_InterruptCallback)
                    {
                        m_InterruptCallback(interrupt_event);
                    }
                    accumulated_data.clear();
                }
            }

            // Handle timeout case - if we have accumulated data but no new data for a while
            else if (!accumulated_data.empty() && m_InterruptCallback)
            {
                InterruptEvent interrupt_event{accumulated_data.size(), std::move(accumulated_data)};
                m_InterruptCallback(interrupt_event);
                accumulated_data.clear();
            }
        }

        close(epoll_fd);
    }
} // namespace UART