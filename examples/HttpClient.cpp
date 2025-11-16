#include "../src/HttpClient.h"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    std::cout << "HTTP Client Example" << std::endl;
    std::cout << "===================" << std::endl;

    // Define handlers
    auto responseHandler = [](void *client, int statusCode, std::string_view statusMessage, std::vector<std::pair<std::string_view, std::string_view>> headers) {
        std::cout << "Response: " << statusCode << " '" << statusMessage << "'" << std::endl;
        for (auto &[key, value] : headers) {
            std::cout << key << ": " << value << std::endl;
        }
        std::cout << std::endl;
    };

    auto dataHandler = [](void *client, std::string_view data, bool fin) {
        if (!data.empty()) {
            std::cout << "Data: " << data << std::endl;
        }
        if (fin) {
            std::cout << "End of response" << std::endl;
        }
    };

    auto failedHandler = []() {
        std::cout << "Request failed!" << std::endl;
    };

    uWS::HttpClientBehavior behavior;
    behavior.response = responseHandler;
    behavior.data = dataHandler;
    behavior.failed = failedHandler;

    uWS::HttpClient client(std::move(behavior));

    try {

    // Example GET request
    std::cout << "Making GET request to localhost..." << std::endl;
    if (client.connect("http://localhost:8080/health")) {
        std::cout << "Connected successfully" << std::endl;
        client.sendRequest();
    } else {
        std::cout << "Failed to connect" << std::endl;
    }

    // Wait a bit
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Example POST request
    std::cout << "\nMaking POST request to httpbin.org..." << std::endl;
    client.setMethod("POST");
    client.addHeader("Content-Type", "application/json");
    client.setBody("{\"key\": \"value\"}");

    if (client.connect("http://localhost:8080/health")) {
        client.sendRequest();
    } else {
        std::cout << "Failed to connect" << std::endl;
    }

    // Wait
    std::this_thread::sleep_for(std::chrono::seconds(2));

    } catch (const std::exception &e) {
        std::cout << "Exception: " << e.what() << std::endl;
    }

    std::cout << "Done" << std::endl;
    return 0;
}

