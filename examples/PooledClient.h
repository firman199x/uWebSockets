#ifndef POOLED_CLIENT_H
#define POOLED_CLIENT_H

#include "../src/ClientApp.h"
#include <iomanip>
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>

class ScopeBenchmark {
  public:
    explicit ScopeBenchmark(const std::string &function_name) :
        m_function_name(function_name)
    { }

    ~ScopeBenchmark()
    {
        auto end = std::chrono::high_resolution_clock::now();
        auto time_taken =
            std::chrono::duration_cast<std::chrono::microseconds>(end - m_start)
                .count();

        if (time_taken < 20) { return; }

        std::cout << std::setw(16) << std::left << "bench_test | "
                  << std::setw(40) << std::left << m_function_name.c_str()
                  << " >>>>>>> " << std::setw(9) << std::right << time_taken
                  << " us" << " (" << time_taken / 1000 << " ms)";

        std::cout << std::endl;
    }

  private:
    std::chrono::time_point<std::chrono::high_resolution_clock> m_start =
        std::chrono::high_resolution_clock::now();
    std::string m_function_name;
};

/// WebSocketClient manages individual WebSocket connections with a shared event loop.
/// Limited to 100 instances to prevent resource exhaustion.
class WebSocketClient {
private:
    size_t index;

    class WebSocketManager {
    public:
        std::vector<std::unique_ptr<uWS::ClientApp>> clients;
        std::thread eventThread;
        std::atomic<bool> running{false};
        std::queue<std::pair<size_t, std::string>> sendQueue;
        std::mutex queueMutex;
        std::condition_variable queueCV;
        std::atomic<int> refCount{0};
        std::chrono::seconds pingInterval{30};
        std::chrono::steady_clock::time_point lastPingTime;

        size_t addClient(uWS::WebSocketClientBehavior&& behavior, const std::string& url);
        void removeClient(size_t index);
        void queueSend(size_t index, const std::string& msg);
        bool isConnected(size_t index);

    private:
        void eventLoop();
    };

    static WebSocketManager manager;

public:
    WebSocketClient(uWS::WebSocketClientBehavior&& behavior, const std::string& url = "");
    ~WebSocketClient();

    void sendMessage(const std::string& msg);
    bool isConnected() const;
};

WebSocketClient::WebSocketManager WebSocketClient::manager{};

WebSocketClient::WebSocketClient(uWS::WebSocketClientBehavior&& behavior, const std::string& url) {
    index = manager.addClient(std::move(behavior), url);
}

WebSocketClient::~WebSocketClient() {
    manager.removeClient(index);
}

void WebSocketClient::sendMessage(const std::string& msg) {
    manager.queueSend(index, msg);
}

bool WebSocketClient::isConnected() const {
    return manager.isConnected(index);
}

size_t WebSocketClient::WebSocketManager::addClient(uWS::WebSocketClientBehavior&& behavior, const std::string& url) {
    if (clients.size() >= 100) {
        throw std::runtime_error("Maximum 100 WebSocketClient instances exceeded.");
    }
    clients.emplace_back(std::make_unique<uWS::ClientApp>(std::move(behavior)));
    size_t idx = clients.size() - 1;
    if (!url.empty()) {
        clients.back()->connect(url);
    }
    if (++refCount == 1 && !running) {
        running = true;
        eventThread = std::thread(&WebSocketClient::WebSocketManager::eventLoop, this);
    }
    return idx;
}

void WebSocketClient::WebSocketManager::removeClient(size_t index) {
    if (index < clients.size()) {
        clients[index].reset();
    }
    if (--refCount == 0) {
        running = false;
        if (eventThread.joinable()) {
            eventThread.join();
        }
    }
}

void WebSocketClient::WebSocketManager::queueSend(size_t index, const std::string& msg) {
    std::lock_guard<std::mutex> lock(queueMutex);
    sendQueue.emplace(index, msg);
    queueCV.notify_one();
}

bool WebSocketClient::WebSocketManager::isConnected(size_t index) {
    return index < clients.size() && clients[index] && clients[index]->isConnected();
}

void WebSocketClient::WebSocketManager::eventLoop() {
    lastPingTime = std::chrono::steady_clock::now();
    while (running) {
        // Check and send pings if interval elapsed
        auto now = std::chrono::steady_clock::now();
        if (now - lastPingTime >= pingInterval) {
            for (size_t i = 0; i < clients.size(); ++i) {
                if (clients[i] && isConnected(i)) {
                    queueSend(i, "ping");
                }
            }
            lastPingTime = now;
        }

        {
            std::unique_lock<std::mutex> lock(queueMutex);
            while (!sendQueue.empty()) {
                auto [idx, msg] = sendQueue.front();
                sendQueue.pop();
                if (idx < clients.size() && clients[idx]) {
                    clients[idx]->sendMessage(msg);
                }
            }
        }

        for (auto &client : clients) {
          if (client) {
            client->run();
          }
        }

        // Small sleep to prevent busy waiting
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
}

#endif // POOLED_CLIENT_H

