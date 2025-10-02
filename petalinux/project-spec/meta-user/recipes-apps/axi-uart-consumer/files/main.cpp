#include <fmt/core.h>
#include <fmt/format.h>
#include <csignal>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <thread>
#include "UartDevice.hpp"

std::atomic<bool> g_running{true};
std::mutex g_output_mutex;

void SignalHandler(int signal)
{
    if (signal == SIGINT || signal == SIGTERM)
    {
        fmt::print("\nShutting down gracefully...\n");
        g_running = false;
    }
}

void DataReceivedCallback(UART::UartDevice *uart, const UART::InterruptEvent &event)
{
    std::lock_guard<std::mutex> lock(g_output_mutex);

    if (!event.data.empty())
    {
        fmt::print("Received {} bytes - echoing back...\n", event.data.size());

        try
        {
            // Echo the data back
            uart->Write(event.data);
            fmt::print("Echoed {} bytes successfully\n", event.data.size());
        }
        catch (const std::exception &ex)
        {
            fmt::print("Error echoing data: {}\n", ex.what());
        }
    }
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
        fmt::print("=== UART Consumer (Echo Server) Started ===\n");
        fmt::print("Device: {}\n", uart_device);
        fmt::print("Press Ctrl+C to exit\n\n");

        std::signal(SIGINT, SignalHandler);
        std::signal(SIGTERM, SignalHandler);

        UART::UartConfig config;
        config.baud_rate = UART::BaudRate::BAUD_230400;
        config.data_bits = UART::DataBits::DATA_8;
        config.parity = UART::Parity::NONE;

        UART::UartDevice uart(uart_device, config);
        fmt::print("UART {} opened successfully\n", uart_device);
        fmt::print("Waiting for data to echo back...\n\n");

        // Register callback with uart pointer for echoing
        uart.RegisterDataCallback([&uart](const UART::InterruptEvent &event)
                                  { DataReceivedCallback(&uart, event); });

        while (g_running)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        uart.DisableInterrupts();

        fmt::print("=== Consumer Shutdown Complete ===\n");
        return 0;
    }
    catch (const std::exception &ex)
    {
        fmt::print("Error: {}\n", ex.what());
        return 1;
    }
}