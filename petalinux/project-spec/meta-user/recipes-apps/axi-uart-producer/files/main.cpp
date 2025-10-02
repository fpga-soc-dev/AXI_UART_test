#include <fmt/core.h>
#include <fmt/format.h>
#include <random>
#include <csignal>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include "UartDevice.hpp"

constexpr size_t TEST_ITERATIONS = 5;
constexpr size_t MIN_RANDOM_DATA_SIZE = 50 * 1024;
constexpr size_t MAX_RANDOM_DATA_SIZE = 51 * 1024;
constexpr auto RESPONSE_TIMEOUT = std::chrono::seconds(5);

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
            byte = 128;
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

std::atomic<bool> g_running{true};
std::mutex g_data_mutex;
std::condition_variable g_data_cv;
std::atomic<bool> g_data_available{false};
std::mutex g_output_mutex;
std::vector<uint8_t> g_received_data;

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
            fmt::print("Received {} bytes from consumer\n", event.bytes_available);
            g_received_data = event.data;
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
    if (argc < 2)
    {
        fmt::print("Usage: {} <uart_device>\n", argv[0]);
        return 1;
    }

    std::string uart_device = argv[1];

    try
    {
        fmt::print("=== UART Producer Started ===\n");
        fmt::print("Device: {}\n", uart_device);

        std::signal(SIGINT, SignalHandler);
        std::signal(SIGTERM, SignalHandler);

        UART::UartConfig config;
        config.baud_rate = UART::BaudRate::BAUD_230400;
        config.data_bits = UART::DataBits::DATA_8;
        config.parity = UART::Parity::NONE;

        UART::UartDevice uart(uart_device, config);
        fmt::print("UART {} opened successfully\n\n", uart_device);

        uart.RegisterDataCallback(DataReceivedCallback);

        for (size_t iteration = 1; iteration <= TEST_ITERATIONS && g_running; ++iteration)
        {
            fmt::print("=== Iteration {}/{} ===\n", iteration, TEST_ITERATIONS);

            auto data_to_send = GenerateRandomData(MIN_RANDOM_DATA_SIZE, MAX_RANDOM_DATA_SIZE);
            fmt::print("Sending {} bytes...\n", data_to_send.size());
            data_to_send.push_back('\n');
            uart.Write(data_to_send);
            fmt::print("Data sent successfully. Waiting for response...\n");

            {
                std::unique_lock<std::mutex> lock(g_data_mutex);
                g_data_available = false;
                g_received_data.clear();

                bool received = g_data_cv.wait_for(lock, RESPONSE_TIMEOUT, []()
                                                   { return g_data_available.load() || !g_running; });

                if (!g_running)
                {
                    break;
                }

                if (received && g_data_available)
                {
                    fmt::print("Response received: {} bytes\n", g_received_data.size());
                    if (g_received_data == data_to_send)
                    {
                        fmt::print("Echo successful!\n");
                    }
                    else
                    {
                        fmt::print("Echo failed!\n");
                        fmt::print("Sent: {} bytes, Received: {} bytes\n",
                                   data_to_send.size(), g_received_data.size());
                    }
                }
                else
                {
                    fmt::print("Timeout waiting for response\n");
                }
            }

            fmt::print("\n");
        }

        uart.DisableInterrupts();

        fmt::print("=== Test Complete ===\n");
        return 0;
    }
    catch (const std::exception &ex)
    {
        fmt::print("Error: {}\n", ex.what());
        return 1;
    }
}