#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include "FileHandle.hpp"

namespace UART
{
    enum class BaudRate : uint32_t
    {
        BAUD_9600 = 9600,
        BAUD_19200 = 19200,
        BAUD_38400 = 38400,
        BAUD_57600 = 57600,
        BAUD_115200 = 115200,
        BAUD_230400 = 230400
    };

    enum class DataBits : uint8_t
    {
        DATA_5 = 5,
        DATA_6 = 6,
        DATA_7 = 7,
        DATA_8 = 8
    };

    enum class Parity : uint8_t
    {
        NONE = 0,
        ODD = 1,
        EVEN = 2
    };

    struct UartConfig
    {
        BaudRate baud_rate = BaudRate::BAUD_115200;
        DataBits data_bits = DataBits::DATA_8;
        Parity parity = Parity::NONE;
        std::chrono::milliseconds read_timeout{1000};
        std::chrono::milliseconds write_timeout{1000};
    };

    struct InterruptEvent
    {
        size_t bytes_available;
        std::vector<uint8_t> data;
    };

    using InterruptCallback = std::function<void(const InterruptEvent &)>;

    class UartDevice
    {
    public:
        explicit UartDevice(const std::string &device_path);
        explicit UartDevice(const std::string &device_path, const UartConfig &config);
        ~UartDevice();

        UartDevice(const UartDevice &) = delete;
        UartDevice &operator=(const UartDevice &) = delete;
        UartDevice(UartDevice &&) noexcept;
        UartDevice &operator=(UartDevice &&) noexcept;

        void Configure(const UartConfig &config);
        UartConfig GetConfig() const;
        bool IsOpen() const;

        size_t Write(std::span<const uint8_t> data);
        std::vector<uint8_t> Read();

        void RegisterDataCallback(const InterruptCallback &callback);
        void DisableInterrupts();

        void Flush();
        size_t BytesAvailable() const;

    private:
        std::unique_ptr<Utils::FileHandle> m_FileHandle;
        std::string m_DevicePath;
        UartConfig m_Config;

        std::jthread m_InterruptThread;
        std::atomic<bool> m_InterruptEnabled{false};
        InterruptCallback m_InterruptCallback;
        mutable std::mutex m_Mutex;

        void ApplyConfig();
        void InterruptHandler(std::stop_token stop_token);
    };
} // namespace UART