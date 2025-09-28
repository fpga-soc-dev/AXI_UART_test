#include <fmt/core.h>
#include <fmt/format.h>
#include <random>
#include <csignal>
#include <atomic>
#include <condition_variable>
#include "UartDevice.hpp"

constexpr size_t TEST_ITERATIONS = 10;
constexpr size_t MAX_RANDOM_DATA_SIZE = 16;
constexpr auto UART0_DEVICE = "/dev/ttyUL1";
constexpr auto UART1_DEVICE = "/dev/ttyUL2";

std::vector<uint8_t> GenerateRandomData(size_t max_size)
{
    static std::random_device rd;
    static std::mt19937 gen(rd());

    // Random size generator
    std::uniform_int_distribution<size_t> size_dis(1, max_size);
    size_t data_size = size_dis(gen);

    // Random data generator
    std::uniform_int_distribution<uint8_t> data_dis(0, 255);

    std::vector<uint8_t> data(data_size);
    for (auto &byte : data)
    {
        byte = data_dis(gen);
    }
    return data;
}

void PrintData(const std::vector<uint8_t> &data, const std::string &prefix)
{
    fmt::print("{}: ", prefix);
    for (const auto &byte : data)
    {
        fmt::print("{:02X} ", byte);
    }
    fmt::print("\n");
}

#ifndef USE_SYNC_TEST
/** Synchronous Test*/
int main()
{
    try
    {
        fmt::print("Starting UART loopback test...\n");
        UART::UartConfig config;
        config.baud_rate = UART::BaudRate::BAUD_115200;
        config.data_bits = UART::DataBits::DATA_8;
        config.parity = UART::Parity::NONE;

        UART::UartDevice uart0(UART0_DEVICE, config);
        UART::UartDevice uart1(UART1_DEVICE, config);

        fmt::print("UART0: {} - Opened\n", UART0_DEVICE);
        fmt::print("UART1: {} - Opened\n\n", UART1_DEVICE);

        for (size_t i = 0; i < TEST_ITERATIONS; ++i)
        {
            fmt::print("=== Synchronous Test Iteration {} ===\n", i + 1);
            auto data0 = GenerateRandomData(MAX_RANDOM_DATA_SIZE);
            auto data1 = GenerateRandomData(MAX_RANDOM_DATA_SIZE);

            PrintData(data0, "Send   : UART0");
            uart0.Flush();
            uart0.Write(data0);

            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            auto received_buffer0 = uart1.Read();

            PrintData(received_buffer0, "Receive: UART1");

            PrintData(data1, "Send   : UART1");
            uart1.Flush();
            uart1.Write(data1);

            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            auto received_buffer1 = uart0.Read();

            PrintData(received_buffer1, "Receive: UART0");

            fmt::print("UART0 Test: {}\n", (data0 == received_buffer0) ? "PASS" : "FAIL");
            fmt::print("UART1 Test: {}\n", (data1 == received_buffer1) ? "PASS" : "FAIL");
        }
    }
    catch (const std::exception &ex)
    {
        fmt::print("Error initializing UART devices: {}\n", ex.what());
        return -1;
    }

    return 0;
}
#else

/** Asynchronous Test*/
void PrintDataAsString(const std::vector<uint8_t> &data, const std::string &prefix)
{
    fmt::print("{}:\n", prefix);
    fmt::print("  Raw Hex: ");
    for (const auto &byte : data)
    {
        fmt::print("{:02X} ", byte);
    }
    fmt::print("\n");

    fmt::print("  As String: \"");
    for (const auto &byte : data)
    {
        if (byte >= 32 && byte <= 126)
        {
            fmt::print("{}", static_cast<char>(byte));
        }
        else if (byte == '\n')
        {
            fmt::print("\\n");
        }
        else if (byte == '\r')
        {
            fmt::print("\\r");
        }
        else if (byte == '\t')
        {
            fmt::print("\\t");
        }
        else if (byte == 0)
        {
            fmt::print("\\0");
        }
        else
        {
            fmt::print("\\x{:02X}", byte);
        }
    }
    fmt::print("\"\n");

    fmt::print("  Size: {} bytes\n", data.size());
}

std::atomic<bool> g_running{true};
std::mutex g_data_mutex;
std::condition_variable g_data_cv;
std::atomic<bool> g_data_available{false};
std::mutex g_output_mutex;

void SignalHandler(int signal)
{
    if (signal == SIGINT || signal == SIGTERM)
    {
        fmt::print("\nShutting down gracefully...\n");
        g_running = false;
        g_data_cv.notify_one();
    }
}

void DataReceivedCallback(const UART::InterruptEvent &event)
{
    {
        std::lock_guard<std::mutex> lock(g_output_mutex);
        if (!event.data.empty())
        {
            PrintDataAsString(event.data, "Received Data");
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_data_mutex);
        g_data_available = true;
    }
    g_data_cv.notify_one();
}

int main(int argc, char *argv[])
{
    std::string uart_device = argv[1];

    try
    {
        fmt::print("=== UART Receiver Application ===\n");
        fmt::print("Device: {}\n", uart_device);
        fmt::print("Press Ctrl+C to exit\n\n");

        std::signal(SIGINT, SignalHandler);
        std::signal(SIGTERM, SignalHandler);

        UART::UartConfig config;
        config.baud_rate = UART::BaudRate::BAUD_115200;
        config.data_bits = UART::DataBits::DATA_8;
        config.parity = UART::Parity::NONE;
        config.read_timeout = std::chrono::milliseconds(1000);
        config.write_timeout = std::chrono::milliseconds(1000);

        UART::UartDevice uart(uart_device, config);
        fmt::print("UART {} opened successfully\n", uart_device);

        uart.RegisterDataCallback(DataReceivedCallback);

        fmt::print("Send data with: echo \"Hello!\" > /dev/ttyUL2\n");

        while (g_running)
        {
            std::unique_lock<std::mutex> lock(g_data_mutex);

            g_data_cv.wait(lock, []()
                           { return g_data_available.load() || !g_running; });

            if (!g_running)
            {
                break;
            }
            g_data_available = false;
        }

        uart.DisableInterrupts();

        return 0;
    }
    catch (const std::exception &ex)
    {
        fmt::print("Error: {}\n", ex.what());
        return 1;
    }
}

#endif
