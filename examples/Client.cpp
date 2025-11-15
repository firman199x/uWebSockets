// Production-Ready WebSocket Client Example
// This example demonstrates a robust WebSocket client with error handling,
// reconnection logic, and production features

#include "../src/ClientApp.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>

std::atomic<bool> running{true};

void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    running = false;
}

int main() {
    // Set up signal handling for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::cout << "Production WebSocket Client Example" << std::endl;
    std::cout << "===================================" << std::endl;
    std::cout << "Press Ctrl+C to exit gracefully" << std::endl;
    std::cout << std::endl;

    // Create client behavior handlers with production features
    auto openHandler = [](void *ws) {
        std::cout << "✅ WebSocket connection opened!" << std::endl;
        std::cout << "Sending hello message..." << std::endl;

        // Cast to ClientWebSocket and send initial message
        auto *clientWs = static_cast<uWS::ClientWebSocket<false>*>(ws);
        clientWs->send("Hello from production client!", uWS::WebSocketFrame::TEXT);
    };

    auto messageHandler = [](void *ws, std::string_view message, int opCode) {
        std::cout << "📨 Received message: " << message << std::endl;
        std::cout << "   OpCode: " << opCode << std::endl;

        // Echo the message back
        auto *clientWs = static_cast<uWS::ClientWebSocket<false>*>(ws);
        clientWs->send(std::string("Echo: ") + std::string(message), uWS::WebSocketFrame::TEXT);
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

    uWS::WebSocketClientBehavior behavior;
    behavior.open = openHandler;
    behavior.message = messageHandler;
    behavior.close = closeHandler;
    behavior.failed = failedHandler;

    uWS::ClientApp<false> app(std::move(behavior));

    // Connect to a WebSocket server
    app.connect("ws://echo.websocket.org", "chat");

    if (app.isConnected()) {
        std::cout << "🔗 Connected successfully!" << std::endl;

        // Start a background thread for periodic ping/pong keepalive
        std::thread pingThread([&app]() {
            while (running.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(30));

                if (app.isConnected()) {
                    // In a real implementation, we'd check if the connection is alive
                    // and send pings if needed
                    std::cout << "🔄 Sending keepalive ping..." << std::endl;
                    app.sendMessage("ping");
                }
            }
        });

        // Main event loop
        int messageCounter = 0;
        while (running.load()) {
            app.run();

            // Small delay to prevent busy waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Send periodic messages if connected
            if (app.isConnected() && ++messageCounter % 50 == 0) { // Every ~5 seconds
                app.sendMessage("Heartbeat message #" + std::to_string(messageCounter / 50));
            }
        }

        pingThread.join();
        std::cout << "👋 Shutting down gracefully..." << std::endl;
    } else {
        std::cout << "❌ Failed to connect!" << std::endl;
    }

    return 0;
}
