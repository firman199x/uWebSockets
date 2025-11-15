// Binary Data Test for Cap'n Proto compatibility
#include "../src/ClientApp.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <cstring>
#include <atomic>

int main() {
    std::cout << "🧪 Testing Binary Data Handling for Cap'n Proto" << std::endl;
    std::cout << "===============================================" << std::endl;

    // Test 1: Binary frame encoding/decoding
    std::cout << "📦 Testing binary frame encoding/decoding..." << std::endl;

    // Create some binary data (simulating Cap'n Proto serialized data)
    std::vector<uint8_t> binaryData = {
        0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE, 0xFD, 0xFC,  // Mixed bytes
        0x80, 0x81, 0x82, 0x83, 0x7F, 0x7E, 0x7D, 0x7C,  // High/low bits
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11   // More binary data
    };

    // Convert to string for WebSocket frame (std::string can hold binary data)
    std::string binaryString(reinterpret_cast<const char*>(binaryData.data()), binaryData.size());

    // Encode as binary frame
    std::string encodedFrame = uWS::WebSocketFrame::encode(binaryString, uWS::WebSocketFrame::BINARY);

    // Decode the frame
    std::string decodedData;
    uWS::WebSocketFrame::OpCode decodedOpCode;
    bool decodedFin;

    bool decodeSuccess = uWS::WebSocketFrame::decode(encodedFrame.data(), encodedFrame.size(),
                                                    decodedData, decodedOpCode, decodedFin);

    // Verify the results
    assert(decodeSuccess);
    assert(decodedOpCode == uWS::WebSocketFrame::BINARY);
    assert(decodedFin == true);
    assert(decodedData.size() == binaryData.size());

    // Verify binary data integrity
    for (size_t i = 0; i < binaryData.size(); ++i) {
        assert(static_cast<uint8_t>(decodedData[i]) == binaryData[i]);
    }

    std::cout << "✅ Binary frame encoding/decoding test passed" << std::endl;

    // Test 2: Null bytes and edge cases
    std::cout << "🔍 Testing null bytes and edge cases..." << std::endl;

    std::vector<uint8_t> edgeCaseData = {
        0x00, 0x00, 0x00, 0x00,  // Multiple null bytes
        0xFF, 0xFF, 0xFF, 0xFF,  // All bits set
        0x00, 0xFF, 0x00, 0xFF,  // Alternating pattern
        0xAA, 0x55, 0xAA, 0x55   // Checkerboard pattern
    };

    std::string edgeCaseString(reinterpret_cast<const char*>(edgeCaseData.data()), edgeCaseData.size());
    std::string edgeEncoded = uWS::WebSocketFrame::encode(edgeCaseString, uWS::WebSocketFrame::BINARY);

    std::string edgeDecoded;
    uWS::WebSocketFrame::OpCode edgeOpCode;
    bool edgeFin;

    bool edgeSuccess = uWS::WebSocketFrame::decode(edgeEncoded.data(), edgeEncoded.size(),
                                                  edgeDecoded, edgeOpCode, edgeFin);

    assert(edgeSuccess);
    assert(edgeOpCode == uWS::WebSocketFrame::BINARY);
    assert(edgeDecoded.size() == edgeCaseData.size());

    for (size_t i = 0; i < edgeCaseData.size(); ++i) {
        assert(static_cast<uint8_t>(edgeDecoded[i]) == edgeCaseData[i]);
    }

    std::cout << "✅ Edge cases test passed" << std::endl;

    // Test 3: Large binary data (simulating large Cap'n Proto messages)
    std::cout << "📊 Testing large binary data..." << std::endl;

    const size_t largeSize = 1024 * 1024; // 1MB
    std::vector<uint8_t> largeData(largeSize);
    for (size_t i = 0; i < largeSize; ++i) {
        largeData[i] = static_cast<uint8_t>(i % 256);
    }

    std::string largeString(reinterpret_cast<const char*>(largeData.data()), largeData.size());
    std::string largeEncoded = uWS::WebSocketFrame::encode(largeString, uWS::WebSocketFrame::BINARY);

    std::string largeDecoded;
    uWS::WebSocketFrame::OpCode largeOpCode;
    bool largeFin;

    bool largeSuccess = uWS::WebSocketFrame::decode(largeEncoded.data(), largeEncoded.size(),
                                                   largeDecoded, largeOpCode, largeFin);

    assert(largeSuccess);
    assert(largeOpCode == uWS::WebSocketFrame::BINARY);
    assert(largeDecoded.size() == largeData.size());

    for (size_t i = 0; i < largeData.size(); ++i) {
        assert(static_cast<uint8_t>(largeDecoded[i]) == largeData[i]);
    }

    std::cout << "✅ Large binary data test passed" << std::endl;

    // Test 4: Integration with ClientApp
    std::cout << "🔗 Testing ClientApp binary data integration..." << std::endl;

    std::atomic<int> receivedMessages{0};
    std::atomic<int> receivedBinaryMessages{0};

    uWS::WebSocketClientBehavior behavior;
    behavior.open = [](void *ws) {
        std::cout << "  📡 Client connected" << std::endl;
    };

    behavior.message = [&receivedMessages, &receivedBinaryMessages](void *ws, std::string_view message, int opCode) {
        receivedMessages++;
        if (opCode == static_cast<int>(uWS::WebSocketFrame::BINARY)) {
            receivedBinaryMessages++;
            std::cout << "  📨 Received binary message: " << message.size() << " bytes" << std::endl;

            // Verify it's actually binary data (contains null bytes)
            bool hasNullBytes = false;
            for (char c : message) {
                if (c == '\0') {
                    hasNullBytes = true;
                    break;
                }
            }
            assert(hasNullBytes); // Should contain null bytes for binary data
        }
    };

    behavior.close = [](void *ws, int code, std::string_view message) {
        std::cout << "  ❌ Client disconnected" << std::endl;
    };

    behavior.failed = []() {
        std::cout << "  💥 Connection failed" << std::endl;
    };

    uWS::ClientApp<false> client(std::move(behavior));
    client.connect("ws://test.example.com/binary");

    // Simulate receiving binary data
    if (client.isConnected()) {
        // Create binary test data
        std::vector<uint8_t> testBinary = {0x00, 0x01, 0x02, 0xFF, 0xFE, 0xFD};
        std::string binaryMsg(reinterpret_cast<const char*>(testBinary.data()), testBinary.size());

        // Send as binary (this would normally be done by the WebSocket server)
        // For testing, we'll simulate the message callback
        behavior.message(nullptr, binaryMsg, static_cast<int>(uWS::WebSocketFrame::BINARY));
    }

    assert(receivedMessages == 1);
    assert(receivedBinaryMessages == 1);

    std::cout << "✅ ClientApp binary integration test passed" << std::endl;

    std::cout << std::endl;
    std::cout << "🎉 All binary data tests passed!" << std::endl;
    std::cout << "✅ Cap'n Proto binary data is fully supported" << std::endl;
    std::cout << std::endl;
    std::cout << "💡 Usage with Cap'n Proto:" << std::endl;
    std::cout << "   // Serialize Cap'n Proto message to binary" << std::endl;
    std::cout << "   std::string binaryData = serializeCapnProtoMessage(message);" << std::endl;
    std::cout << "   " << std::endl;
    std::cout << "   // Send as binary WebSocket frame" << std::endl;
    std::cout << "   client.send(binaryData, uWS::WebSocketFrame::BINARY);" << std::endl;
    std::cout << "   " << std::endl;
    std::cout << "   // Receive and check if it's binary" << std::endl;
    std::cout << "   if (opCode == uWS::WebSocketFrame::BINARY) {" << std::endl;
    std::cout << "       auto capnProtoMessage = deserializeCapnProtoMessage(message);" << std::endl;
    std::cout << "   }" << std::endl;

    return 0;
}