#include "../src/HttpClient.h"
#include <iostream>
#include <vector>
#include <future>
#include <chrono>

int main() {
    std::cout << "HTTP Client Example - Async Interface" << std::endl;
    std::cout << "=====================================" << std::endl;

    const int num_requests = 10;
    std::vector<std::future<uWS::HttpReply>> futures;

    // Start multiple async requests
    for (int i = 0; i < num_requests; ++i) {
        if (i == 5) 
        futures.push_back(uWS::HttpClientPool::HttpRequest("GET", "http://localhost:8081/invalid"));
        else
        futures.push_back(uWS::HttpClientPool::HttpRequest("GET", "http://localhost:8080/health"));
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    // Wait for all responses
    for (auto &f : futures) {
        auto reply = f.get();
        std::cout << reply.status_code << " reply: " << reply.body << "\n";
        // Process reply if needed
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    std::cout << "All " << num_requests << " requests completed in " << total_duration.count() << " ms" << std::endl;
    std::cout << "Rate: " << (num_requests * 1000.0 / total_duration.count()) << " req/s" << std::endl;
    return 0;
}

