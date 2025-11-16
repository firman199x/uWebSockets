#include "PooledClient.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <chrono>
#include <iomanip>

std::atomic<bool> globalRunning{true};

void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    globalRunning = false;
}

int main() {
    // Set up signal handling for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::cout << "Production WebSocket Client Example" << std::endl;
    std::cout << "===================================" << std::endl;
    std::cout << "Press Ctrl+C to exit gracefully" << std::endl;
    std::cout << std::endl;

    // Define handlers as before
    auto openHandler = [](void *ws) {
        std::cout << "✅ WebSocket connection opened!" << std::endl;
        // Note: Since we can't call sendMessage here directly, we queue it
        // But for simplicity, we'll send from main after connection
    };

    static std::chrono::system_clock::time_point sendTime;
    auto messageHandler = [](void *ws, std::string_view message, int opCode) {
        if (opCode != 1) return; // Only handle TEXT messages
        auto recvTime = std::chrono::system_clock::now();
        auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(recvTime - sendTime).count();
        auto elapsed_ms = elapsed_us / 1000;
        auto recv_us = std::chrono::duration_cast<std::chrono::microseconds>(recvTime.time_since_epoch()).count();
        auto seconds = recv_us / 1000000;
        auto micros = recv_us % 1000000;
        std::time_t t = seconds;
        std::cout << "📨 Received at " << std::put_time(std::gmtime(&t), "%F %T") << "." << std::setfill('0') << std::setw(6) << micros << " (" << elapsed_us << "us after send): " << message;
        if (elapsed_ms > 100) std::cout << " [SLOW]";
        std::cout << std::endl;
        // Echoing would require access to the client instance, which we don't have here
        // So, perhaps store a reference or use a global
        // For this example, we'll skip echoing and just log
    };

    auto closeHandler = [](void *ws, int code, std::string_view message) {
        std::cout << "❌ WebSocket connection closed!" << std::endl;
        std::cout << "   Code: " << code << std::endl;
        if (!message.empty()) {
            std::cout << "   Message: " << message << std::endl;
        }
    };

    auto failedHandler = []() {
        std::cout << "❌ Connection failed!" << std::endl;
    };

    try {
        const std::string url = "ws://localhost:9001";
        int reconnect_attempts = 0;
        const int max_attempts = 5;
        std::chrono::milliseconds backoff(1000);

        WebSocketClient* client_ptr = nullptr;
        bool connected = false;

        while (reconnect_attempts < max_attempts && !connected) {
            if (reconnect_attempts > 0) {
                std::cout << "Retrying connection in " << backoff.count() << " ms..." << std::endl;
                std::this_thread::sleep_for(backoff);
                backoff = std::min(backoff * 2, std::chrono::milliseconds(30000)); // cap at 30s
            }

            try {
                uWS::WebSocketClientBehavior behavior;
                behavior.open = openHandler;
                behavior.message = messageHandler;
                behavior.close = closeHandler;
                behavior.failed = failedHandler;

                client_ptr = new WebSocketClient(std::move(behavior), url);
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (client_ptr->isConnected()) {
                    connected = true;
                    std::cout << "🔗 Connected successfully!" << std::endl;
                } else {
                    delete client_ptr;
                    client_ptr = nullptr;
                    reconnect_attempts++;
                }
            } catch (const std::runtime_error& e) {
                std::cout << "Connection attempt failed: " << e.what() << std::endl;
                if (client_ptr) {
                    delete client_ptr;
                    client_ptr = nullptr;
                }
                reconnect_attempts++;
            }
        }

        if (!connected) {
            std::cout << "❌ Failed to connect after " << max_attempts << " attempts!" << std::endl;
            return 1;
        }

        WebSocketClient& client = *client_ptr;

            // Send initial message
            std::string helloMsg = "Hello from production client!";
            sendTime = std::chrono::system_clock::now();
            auto send_us = std::chrono::duration_cast<std::chrono::microseconds>(sendTime.time_since_epoch()).count();
            auto seconds = send_us / 1000000;
            auto micros = send_us % 1000000;
            std::time_t t = seconds;
            std::cout << "📤 Queueing at " << std::put_time(std::gmtime(&t), "%F %T") << "." << std::setfill('0') << std::setw(6) << micros << ": " << helloMsg << std::endl;
        client.sendMessage(helloMsg);

        auto sent = 0;
        while (sent < 10) {
            auto msg = "Hello from production client! " + std::to_string(sent);
            client.sendMessage(msg);
            sent++;
        }

        std::this_thread::sleep_for(std::chrono::seconds(20));

        std::cout << "👋 Shutting down gracefully..." << std::endl;
        delete client_ptr;
    } catch (const std::runtime_error& e) {
        std::cout << "Error: " << e.what() << std::endl;
    }

    return 0;
}

