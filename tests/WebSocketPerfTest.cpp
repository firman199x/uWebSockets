// WebSocket Performance Benchmark
#include "../src/ClientApp.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <string>

int main() {
    std::cout << "WebSocket Client Performance Analysis" << std::endl;
    std::cout << "====================================" << std::endl;

    // Test message sizes
    std::vector<size_t> messageSizes = {64, 512, 4096, 32768, 131072};

    for (size_t size : messageSizes) {
        // Create test message
        std::string testMessage(size, 'A');
        for (size_t i = 0; i < size; ++i) {
            testMessage[i] = 'A' + (i % 26);
        }

        // Benchmark encoding
        auto start = std::chrono::high_resolution_clock::now();
        const int iterations = 10000;

        size_t totalEncodedSize = 0;
        for (int i = 0; i < iterations; ++i) {
            std::string encoded = uWS::WebSocketFrame::encode(testMessage, uWS::WebSocketFrame::TEXT);
            totalEncodedSize += encoded.size();
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto encodingTime = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

        // Benchmark decoding
        std::string encodedSample = uWS::WebSocketFrame::encode(testMessage, uWS::WebSocketFrame::TEXT);
        start = std::chrono::high_resolution_clock::now();

        std::string decodedMessage;
        uWS::WebSocketFrame::OpCode opCode;
        bool fin;
        int decodeSuccesses = 0;

        for (int i = 0; i < iterations; ++i) {
            decodedMessage.clear();
            if (uWS::WebSocketFrame::decode(encodedSample.data(), encodedSample.size(),
                                           decodedMessage, opCode, fin)) {
                decodeSuccesses++;
            }
        }

        end = std::chrono::high_resolution_clock::now();
        auto decodingTime = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

        // Calculate metrics
        double encodingThroughput = (double)(size * iterations) / encodingTime.count() * 1000000 / (1024*1024); // MB/s
        double decodingThroughput = (double)(size * iterations) / decodingTime.count() * 1000000 / (1024*1024); // MB/s

        std::cout << "Message size: " << size << " bytes" << std::endl;
        std::cout << "  Encoding: " << encodingTime.count() / iterations << " μs/op, "
                  << encodingThroughput << " MB/s" << std::endl;
        std::cout << "  Decoding: " << decodingTime.count() / iterations << " μs/op, "
                  << decodingThroughput << " MB/s" << std::endl;
        std::cout << "  Overhead: " << (double)encodedSample.size() / size << "x" << std::endl;
        std::cout << std::endl;
    }

    return 0;
}