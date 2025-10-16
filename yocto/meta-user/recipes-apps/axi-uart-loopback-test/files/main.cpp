#include <fmt/core.h>
#include <fmt/format.h>
#include <random>
#include <csignal>
#include <atomic>
#include <condition_variable>
#include "UartDevice.hpp"

constexpr size_t TEST_ITERATIONS = 5;
constexpr size_t MIN_RANDOM_DATA_SIZE = 20 * 1024;
constexpr size_t MAX_RANDOM_DATA_SIZE = 25 * 1024;
constexpr auto UART0_DEVICE = "/dev/ttyUL1";
constexpr auto UART1_DEVICE = "/dev/ttyUL2";

std::vector<uint8_t> GenerateRandomData(size_t min_size, size_t max_size)
{
    static std::random_device rd;
    static std::mt19937 gen(rd());

    // Random size generator
    std::uniform_int_distribution<size_t> size_dis(min_size, max_size);
    size_t data_size = size_dis(gen);

    // Random data generator
    std::uniform_int_distribution<uint8_t> data_dis(0, 255);

    std::vector<uint8_t> data(data_size);
    for (auto &byte : data)
    {
        byte = data_dis(gen);
        if (byte == '\n')
        {
            byte = 180; // Replace newline with 180 (or any other value you prefer)
        }
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

int main()
{
    try
    {
        fmt::print("Starting UART loopback test...\n");
        UART::UartConfig config;
        config.baud_rate = UART::BaudRate::BAUD_230400;
        config.data_bits = UART::DataBits::DATA_8;
        config.parity = UART::Parity::NONE;

        UART::UartDevice uart0(UART0_DEVICE, config);
        UART::UartDevice uart1(UART1_DEVICE, config);

        fmt::print("UART0: {} - Opened\n", UART0_DEVICE);
        fmt::print("UART1: {} - Opened\n\n", UART1_DEVICE);

        for (size_t i = 0; i < TEST_ITERATIONS; ++i)
        {
            fmt::print("=== Synchronous Test Iteration {} ===\n", i + 1);
            auto data = GenerateRandomData(MIN_RANDOM_DATA_SIZE, MAX_RANDOM_DATA_SIZE);
            data.push_back('\n'); // delimiter

            uart0.Flush();
            uart1.Flush();

            // PrintData(data, "Send   : UART0");
            uart0.Write(data);
            fmt::print("UART0 wrote {} bytes\n", data.size());

            auto received_buffer0 = uart1.Read();
            fmt::print("UART1 read {} bytes\n", received_buffer0.size());

            // PrintData(received_buffer0, "Receive: UART1");

            // PrintData(data1, "Send   : UART1");
            uart1.Write(received_buffer0);
            fmt::print("UART1 wrote {} bytes\n", received_buffer0.size());

            auto received_buffer1 = uart0.Read();

            // PrintData(received_buffer1, "Receive: UART0");
            fmt::print("UART0 read {} bytes\n", received_buffer1.size());
            fmt::print("UART Test: {}\n", (data == received_buffer1) ? "PASS" : "FAIL");
        }
    }
    catch (const std::exception &ex)
    {
        fmt::print("Error initializing UART devices: {}\n", ex.what());
        return -1;
    }

    return 0;
}
