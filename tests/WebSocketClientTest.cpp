// WebSocket Frame Test
// Tests the WebSocket frame encoding and decoding functionality

#include "../src/ClientApp.h"
#include <iostream>
#include <cassert>

int main() {
    std::cout << "WebSocket Frame Test" << std::endl;
    std::cout << "====================" << std::endl;

    // Test encoding a simple text message
    std::string originalMessage = "Hello, WebSocket!";
    std::string encodedFrame = uWS::WebSocketFrame::encode(originalMessage, uWS::WebSocketFrame::TEXT);

    std::cout << "Original message: " << originalMessage << std::endl;
    std::cout << "Encoded frame size: " << encodedFrame.size() << " bytes" << std::endl;

    // Test decoding the frame
    std::string decodedMessage;
    uWS::WebSocketFrame::OpCode decodedOpCode;
    bool decodedFin;

    bool decodeSuccess = uWS::WebSocketFrame::decode(
        encodedFrame.data(),
        encodedFrame.size(),
        decodedMessage,
        decodedOpCode,
        decodedFin
    );

    if (decodeSuccess) {
        std::cout << "✅ Decode successful!" << std::endl;
        std::cout << "Decoded message: " << decodedMessage << std::endl;
        std::cout << "Decoded OpCode: " << decodedOpCode << std::endl;
        std::cout << "Decoded FIN: " << (decodedFin ? "true" : "false") << std::endl;

        // Verify the results
        assert(decodedMessage == originalMessage);
        assert(decodedOpCode == uWS::WebSocketFrame::TEXT);
        assert(decodedFin == true);

        std::cout << "✅ All assertions passed!" << std::endl;
    } else {
        std::cout << "❌ Decode failed!" << std::endl;
        return 1;
    }

    // Test with different OpCodes
    std::string binaryMessage = "Binary data";
    std::string binaryFrame = uWS::WebSocketFrame::encode(binaryMessage, uWS::WebSocketFrame::BINARY);

    std::string decodedBinaryMessage;
    uWS::WebSocketFrame::OpCode decodedBinaryOpCode;
    bool decodedBinaryFin;

    bool binaryDecodeSuccess = uWS::WebSocketFrame::decode(
        binaryFrame.data(),
        binaryFrame.size(),
        decodedBinaryMessage,
        decodedBinaryOpCode,
        decodedBinaryFin
    );

    if (binaryDecodeSuccess) {
        assert(decodedBinaryMessage == binaryMessage);
        assert(decodedBinaryOpCode == uWS::WebSocketFrame::BINARY);
        assert(decodedBinaryFin == true);
        std::cout << "✅ Binary frame test passed!" << std::endl;
    } else {
        std::cout << "❌ Binary frame test failed!" << std::endl;
        return 1;
    }

    // Test URL parsing
    uWS::ParsedUrl url1 = uWS::ParsedUrl::parse("ws://example.com:8080/chat");
    assert(url1.host == "example.com");
    assert(url1.port == "8080");
    assert(url1.path == "/chat");
    assert(url1.ssl == false);

    uWS::ParsedUrl url2 = uWS::ParsedUrl::parse("wss://secure.example.com/websocket");
    assert(url2.host == "secure.example.com");
    assert(url2.port == "443");
    assert(url2.path == "/websocket");
    assert(url2.ssl == true);

    uWS::ParsedUrl url3 = uWS::ParsedUrl::parse("ws://localhost");
    assert(url3.host == "localhost");
    assert(url3.port == "80");
    assert(url3.path == "/");
    assert(url3.ssl == false);

    std::cout << "✅ URL parsing tests passed!" << std::endl;

    std::cout << "🎉 All tests passed successfully!" << std::endl;
    return 0;
}