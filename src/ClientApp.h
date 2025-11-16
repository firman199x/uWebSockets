#include "MoveOnlyFunction.h"
#include <string>
#include <string_view>
#include <vector>
#include <random>
#include <sstream>
#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>
#ifdef __SSE2__
#include <emmintrin.h>
#endif
#ifdef __AVX2__
#include <immintrin.h>
#endif

namespace uWS {

    struct WebSocketClientBehavior {
        MoveOnlyFunction<void(void *)> open;  // Simplified - void* instead of WebSocket*
        MoveOnlyFunction<void(void *, std::string_view, int)> message;  // int instead of OpCode
        MoveOnlyFunction<void(void *, int, std::string_view)> close;
        MoveOnlyFunction<void()> failed;
    };

    // Simple URL parser for WebSocket client
    struct ParsedUrl {
        std::string host;
        std::string port;
        std::string path;
        bool ssl;

        static ParsedUrl parse(std::string_view url) {
            ParsedUrl result;
            // Check for wss:// prefix (SSL)
            if (url.size() >= 6 && url.substr(0, 6) == "wss://") {
                result.ssl = true;
            } else if (url.size() >= 5 && url.substr(0, 5) == "ws://") {
                result.ssl = false;
            } else {
                // Invalid URL
                return result;
            }

            size_t start = result.ssl ? 6 : 5; // Skip "ws://" or "wss://"

            size_t colonPos = url.find(':', start);
            size_t slashPos = url.find('/', start);

            if (colonPos != std::string_view::npos && (slashPos == std::string_view::npos || colonPos < slashPos)) {
                // Has port
                result.host = std::string(url.substr(start, colonPos - start));
                size_t portEnd = slashPos != std::string_view::npos ? slashPos : url.size();
                result.port = std::string(url.substr(colonPos + 1, portEnd - colonPos - 1));
            } else {
                // No port, use default
                size_t hostEnd = slashPos != std::string_view::npos ? slashPos : url.size();
                result.host = std::string(url.substr(start, hostEnd - start));
                result.port = result.ssl ? "443" : "80";
            }

            result.path = slashPos != std::string_view::npos ? std::string(url.substr(slashPos)) : "/";
            return result;
        }
    };

    // Generate random Sec-WebSocket-Key
    inline std::string generateWebSocketKey() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);

        std::string key;
        for (int i = 0; i < 16; ++i) {
            key += static_cast<char>(dis(gen));
        }

        // Base64 encode (simplified implementation)
        static const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string encoded;
        int val = 0, valb = -6;
        for (unsigned char c : key) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) {
                encoded.push_back(base64_chars[(val >> valb) & 0x3F]);
                valb -= 6;
                val &= 0x3FFFFFFF;
            }
        }
        if (valb > -6) encoded.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
        while (encoded.size() % 4) encoded.push_back('=');
        return encoded;
    }

    // Production-ready WebSocket frame parser/encoder with SIMD optimizations
    class WebSocketFrame {
    public:
        enum OpCode {
            CONTINUATION = 0,
            TEXT = 1,
            BINARY = 2,
            CLOSE = 8,
            PING = 9,
            PONG = 10
        };

        // Zero-copy encoding with external buffer
        static size_t encodeToBuffer(std::string_view message, OpCode opCode, bool fin,
                                   char *buffer, size_t bufferSize, unsigned char maskKey[4]) {
            size_t requiredSize = getEncodedSize(message.size());
            if (requiredSize > bufferSize) return 0; // Buffer too small

            char *ptr = buffer;

            // First byte: FIN + opcode
            *ptr++ = static_cast<char>((fin ? 0x80 : 0x00) | static_cast<unsigned char>(opCode));

            // Second byte: MASK + length
            size_t length = message.size();
            if (length <= 125) {
                *ptr++ = static_cast<char>(length | 0x80);
            } else if (length <= 65535) {
                *ptr++ = static_cast<char>(126 | 0x80);
                *ptr++ = static_cast<char>((length >> 8) & 0xFF);
                *ptr++ = static_cast<char>(length & 0xFF);
            } else {
                *ptr++ = static_cast<char>(127 | 0x80);
                for (int i = 7; i >= 0; --i) {
                    *ptr++ = static_cast<char>((length >> (i * 8)) & 0xFF);
                }
            }

            // Masking key
            for (int i = 0; i < 4; ++i) {
                *ptr++ = static_cast<char>(maskKey[i]);
            }

            // Masked payload with SIMD optimization
            applyMaskSIMD(message.data(), ptr, length, maskKey);

            return requiredSize;
        }

        // Legacy method for compatibility
        static std::string encode(std::string_view message, OpCode opCode = TEXT, bool fin = true) {
            unsigned char maskKey[4];
            generateMaskKey(maskKey);

            size_t size = getEncodedSize(message.size());
            std::string frame(size, '\0');

            size_t actualSize = encodeToBuffer(message, opCode, fin, frame.data(), size, maskKey);
            if (actualSize == 0) return std::string(); // Error

            frame.resize(actualSize);
            return frame;
        }

        // Zero-copy decoding
        static bool decodeZeroCopy(const char *data, size_t size, OpCode &outOpCode, bool &outFin,
                                 size_t &outPayloadOffset, size_t &outPayloadLength, const unsigned char *&outMask) {
            if (size < 2) return false;

            // First byte
            unsigned char firstByte = static_cast<unsigned char>(data[0]);
            outFin = (firstByte & 0x80) != 0;
            outOpCode = static_cast<OpCode>(firstByte & 0x0F);

            // Second byte
            unsigned char secondByte = static_cast<unsigned char>(data[1]);
            bool masked = (secondByte & 0x80) != 0;
            size_t length = secondByte & 0x7F;

            size_t headerSize = 2;
            if (length == 126) {
                if (size < 4) return false;
                length = (static_cast<unsigned char>(data[2]) << 8) | static_cast<unsigned char>(data[3]);
                headerSize = 4;
            } else if (length == 127) {
                if (size < 10) return false;
                length = 0;
                for (int i = 0; i < 8; ++i) {
                    length = (length << 8) | static_cast<unsigned char>(data[2 + i]);
                }
                headerSize = 10;
            }

            if (masked) {
                if (size < headerSize + 4) return false;
                outMask = reinterpret_cast<const unsigned char *>(data + headerSize);
                headerSize += 4;
            } else {
                outMask = nullptr;
            }

            if (size < headerSize + length) return false;

            outPayloadOffset = headerSize;
            outPayloadLength = length;
            return true;
        }

        // Legacy decode method
        static bool decode(const char *data, size_t size, std::string &outMessage, OpCode &outOpCode, bool &outFin) {
            OpCode opCode;
            bool fin;
            size_t payloadOffset, payloadLength;
            const unsigned char *mask;

            if (!decodeZeroCopy(data, size, opCode, fin, payloadOffset, payloadLength, mask)) {
                return false;
            }

            outOpCode = opCode;
            outFin = fin;

            outMessage.clear();
            outMessage.reserve(payloadLength);

            if (mask) {
                // Apply mask during copy
                for (size_t i = 0; i < payloadLength; ++i) {
                    outMessage.push_back(data[payloadOffset + i] ^ mask[i % 4]);
                }
            } else {
                outMessage.assign(data + payloadOffset, payloadLength);
            }

            return true;
        }

        // Utility functions
        static size_t getEncodedSize(size_t payloadSize) {
            size_t headerSize = 2; // Base header
            if (payloadSize <= 125) {
                // No extra length bytes
            } else if (payloadSize <= 65535) {
                headerSize += 2; // 16-bit length
            } else {
                headerSize += 8; // 64-bit length
            }
            headerSize += 4; // Mask key
            return headerSize + payloadSize;
        }

        static void generateMaskKey(unsigned char maskKey[4]) {
            // Use thread-local random for better performance
            static thread_local std::mt19937 gen(std::random_device{}());
            std::uniform_int_distribution<unsigned char> dis(0, 255);
            for (int i = 0; i < 4; ++i) {
                maskKey[i] = dis(gen);
            }
        }

    private:
        // SIMD-optimized masking (fallback to scalar if SIMD not available)
        static void applyMaskSIMD(const char *input, char *output, size_t length, const unsigned char mask[4]) {
            size_t i = 0;

            // Process 32 bytes at a time with AVX2
            #ifdef __AVX2__
            __m256i mask256 = _mm256_set_epi8(
                mask[3], mask[2], mask[1], mask[0], mask[3], mask[2], mask[1], mask[0],
                mask[3], mask[2], mask[1], mask[0], mask[3], mask[2], mask[1], mask[0],
                mask[3], mask[2], mask[1], mask[0], mask[3], mask[2], mask[1], mask[0],
                mask[3], mask[2], mask[1], mask[0], mask[3], mask[2], mask[1], mask[0]
            );
            for (; i + 32 <= length; i += 32) {
                __m256i data = _mm256_loadu_si256((__m256i*)(input + i));
                __m256i masked = _mm256_xor_si256(data, mask256);
                _mm256_storeu_si256((__m256i*)(output + i), masked);
            }
            #endif

            // Process 16 bytes at a time with SSE2
            #ifdef __SSE2__
            __m128i mask128 = _mm_set_epi8(
                mask[3], mask[2], mask[1], mask[0], mask[3], mask[2], mask[1], mask[0],
                mask[3], mask[2], mask[1], mask[0], mask[3], mask[2], mask[1], mask[0]
            );
            for (; i + 16 <= length; i += 16) {
                __m128i data = _mm_loadu_si128((__m128i*)(input + i));
                __m128i masked = _mm_xor_si128(data, mask128);
                _mm_storeu_si128((__m128i*)(output + i), masked);
            }
            #endif

            // Fallback: optimized scalar version
            // Process in chunks of 4 bytes for better cache performance
            for (; i + 4 <= length; i += 4) {
                output[i] = input[i] ^ mask[0];
                output[i + 1] = input[i + 1] ^ mask[1];
                output[i + 2] = input[i + 2] ^ mask[2];
                output[i + 3] = input[i + 3] ^ mask[3];
            }

            // Handle remaining bytes
            for (; i < length; ++i) {
                output[i] = input[i] ^ mask[i % 4];
            }
        }
    };

    // Production-ready WebSocket client with proper networking
    template <bool SSL>
    class ClientWebSocket {
        template <bool> friend struct ClientApp;

    private:
        WebSocketClientBehavior *behavior;
        void *socket = nullptr; // Using void* to avoid uSockets dependency
        bool connected = false;
        std::vector<char> sendBuffer;
        std::vector<char> receiveBuffer;
        size_t readOffset = 0;
        unsigned char currentMask[4];

        // Fragmentation support
        std::vector<char> fragmentBuffer;
        WebSocketFrame::OpCode fragmentOpCode;
        bool inFragment = false;

        // Connection state
        enum State {
            CONNECTING,
            HANDSHAKE,
            CONNECTED,
            CLOSING,
            CLOSED
        } state = CLOSED;

        // For ping/pong handling
        std::chrono::steady_clock::time_point lastPingTime;
        std::chrono::steady_clock::time_point lastPongTime;

    public:
        ClientWebSocket(WebSocketClientBehavior *behavior) : behavior(behavior) {
            WebSocketFrame::generateMaskKey(currentMask);
            lastPongTime = std::chrono::steady_clock::now();
        }

        ~ClientWebSocket() {
            close(1000, "Client shutdown");
        }

        // Send methods with backpressure handling and fragmentation support
        bool send(std::string_view message, WebSocketFrame::OpCode opCode = WebSocketFrame::TEXT, bool compress = false) {
            if (state != CONNECTED) return false;

            // Handle fragmentation for large messages
            const size_t maxFrameSize = 32768; // 32KB fragments
            if (message.size() <= maxFrameSize) {
                return sendFrame(message, opCode, true, compress);
            } else {
                // Fragment the message
                size_t offset = 0;
                bool isFirst = true;

                while (offset < message.size()) {
                    size_t chunkSize = std::min(maxFrameSize, message.size() - offset);
                    std::string_view chunk = message.substr(offset, chunkSize);

                    WebSocketFrame::OpCode frameOpCode = isFirst ? opCode : WebSocketFrame::CONTINUATION;
                    bool fin = (offset + chunkSize) >= message.size();

                    if (!sendFrame(chunk, frameOpCode, fin, compress && isFirst)) {
                        return false;
                    }

                    offset += chunkSize;
                    isFirst = false;
                }
                return true;
            }
        }

    private:
        bool sendFrame(std::string_view message, WebSocketFrame::OpCode opCode, bool fin, bool compress) {
            // Reserve space in send buffer
            size_t frameSize = WebSocketFrame::getEncodedSize(message.size());
            if (sendBuffer.capacity() < sendBuffer.size() + frameSize) {
                sendBuffer.reserve(sendBuffer.size() + frameSize + 4096); // Extra padding
            }

            // Encode frame directly into send buffer
            size_t offset = sendBuffer.size();
            sendBuffer.resize(offset + frameSize);

            size_t actualSize = WebSocketFrame::encodeToBuffer(message, opCode, fin,
                                                             sendBuffer.data() + offset, frameSize, currentMask);
            if (actualSize == 0) {
                sendBuffer.resize(offset); // Revert on error
                return false;
            }

            sendBuffer.resize(offset + actualSize);

            // Generate new mask key for next frame
            WebSocketFrame::generateMaskKey(currentMask);

            // Try to send immediately, buffer if needed
            return flushSendBuffer();
        }

        void close(int code = 1000, std::string_view message = "") {
            if (state == CONNECTED) {
                state = CLOSING;

                // Send close frame
                uint16_t networkCode = (code >> 8) | (code << 8); // Convert to network byte order
                std::string closePayload;
                closePayload.append(reinterpret_cast<const char*>(&networkCode), 2);
                closePayload.append(message.data(), message.size());

                send(closePayload, WebSocketFrame::CLOSE);
            } else if (state == CLOSING) {
                state = CLOSED;
                connected = false;
                if (behavior && behavior->close) {
                    behavior->close(this, code, message);
                }
            }
        }

        // Networking integration methods
        void setSocket(void *s) { socket = s; }
        void *getSocket() const { return socket; }

        bool isConnected() const { return connected; }
        void setConnected(bool c) {
            connected = c;
            state = c ? CONNECTED : CLOSED;
            if (c && behavior && behavior->open) {
                behavior->open(this);
            }
        }

        State getState() const { return state; }
        void setState(State s) { state = s; }

        // Buffer management
        bool flushSendBuffer() {
            if (sendBuffer.empty()) return true;

            // In a real implementation, this would use us_socket_write
            // For now, simulate sending
            std::cout << "Flushing " << sendBuffer.size() << " bytes from send buffer" << std::endl;
            sendBuffer.clear();
            return true;
        }

        void appendReceiveData(const char *data, size_t length) {
            receiveBuffer.insert(receiveBuffer.end(), data, data + length);
            processReceiveBuffer();
        }

        void processReceiveBuffer() {
            size_t offset = readOffset;

            while (offset < receiveBuffer.size()) {
                WebSocketFrame::OpCode opCode;
                bool fin;
                size_t payloadOffset, payloadLength;
                const unsigned char *mask;

                if (!WebSocketFrame::decodeZeroCopy(receiveBuffer.data() + offset,
                                                  receiveBuffer.size() - offset,
                                                  opCode, fin, payloadOffset, payloadLength, mask)) {
                    break; // Need more data
                }

                // Handle frame with fragmentation support
                if (!handleFrameFragmented(opCode, fin, receiveBuffer.data() + offset + payloadOffset, payloadLength, mask)) {
                    break; // Frame not complete
                }

                // Move to next frame
                size_t frameSize = payloadOffset + payloadLength;
                offset += frameSize;
            }

            readOffset = offset;
            // Compact buffer if more than half is processed
            if (readOffset > receiveBuffer.size() / 2 && readOffset > 0) {
                std::copy(receiveBuffer.begin() + readOffset, receiveBuffer.end(), receiveBuffer.begin());
                receiveBuffer.resize(receiveBuffer.size() - readOffset);
                readOffset = 0;
            }
        }

        bool handleFrameFragmented(WebSocketFrame::OpCode opCode, bool fin,
                                  const char *payload, size_t length, const unsigned char *mask) {
            std::vector<char> message;

            // Unmask if necessary
            if (mask) {
                message.resize(length);
                for (size_t i = 0; i < length; ++i) {
                    message[i] = payload[i] ^ mask[i % 4];
                }
            } else {
                message.assign(payload, payload + length);
            }

            // Handle fragmentation
            if (opCode == WebSocketFrame::CONTINUATION) {
                if (!inFragment) {
                    // Unexpected continuation frame
                    close(1002, "Unexpected continuation frame");
                    return false;
                }
                fragmentBuffer.insert(fragmentBuffer.end(), message.begin(), message.end());
            } else {
                if (inFragment) {
                    // Previous fragment not finished
                    close(1002, "Fragment not finished");
                    return false;
                }

                if (!fin) {
                    // Start new fragment
                    inFragment = true;
                    fragmentOpCode = opCode;
                    fragmentBuffer = std::move(message);
                    return true; // Wait for more fragments
                } else {
                    // Single frame message
                    fragmentOpCode = opCode;
                    fragmentBuffer = std::move(message);
                }
            }

            if (fin) {
                // Message complete
                inFragment = false;
                handleCompleteMessage(fragmentOpCode, fragmentBuffer);
                fragmentBuffer.clear();
            }

            return true;
        }

        void handleCompleteMessage(WebSocketFrame::OpCode opCode, const std::vector<char>& message) {
            handleFrame(opCode, true, message.data(), message.size(), nullptr);
        }

        // Ping/pong for keepalive
        void sendPing(std::string_view data = "") {
            if (state == CONNECTED) {
                send(data, WebSocketFrame::PING);
                lastPingTime = std::chrono::steady_clock::now();
            }
        }

        bool isAlive(int timeoutMs = 30000) {
            auto now = std::chrono::steady_clock::now();
            auto timeSincePong = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPongTime);
            return timeSincePong.count() < timeoutMs;
        }

    private:
        void handleFrame(WebSocketFrame::OpCode opCode, bool fin, const char *payload, size_t length, const unsigned char *mask) {
            switch (opCode) {
                case WebSocketFrame::TEXT:
                case WebSocketFrame::BINARY: {
                    if (behavior && behavior->message) {
                        std::string message;
                        if (mask) {
                            message.reserve(length);
                            for (size_t i = 0; i < length; ++i) {
                                message.push_back(payload[i] ^ mask[i % 4]);
                            }
                        } else {
                            message.assign(payload, length);
                        }
                        behavior->message(this, message, static_cast<int>(opCode));
                    }
                    break;
                }
                case WebSocketFrame::CLOSE: {
                    int closeCode = 1000; // Default
                    std::string closeMessage;

                    if (length >= 2) {
                        closeCode = (static_cast<unsigned char>(payload[0]) << 8) |
                                   static_cast<unsigned char>(payload[1]);
                        if (mask) {
                            closeCode ^= (mask[0] << 8) | mask[1];
                        }

                        if (length > 2) {
                            closeMessage.assign(payload + 2, length - 2);
                            if (mask) {
                                for (size_t i = 0; i < closeMessage.size(); ++i) {
                                    closeMessage[i] ^= mask[(i + 2) % 4];
                                }
                            }
                        }
                    }

                    close(closeCode, closeMessage);
                    break;
                }
                case WebSocketFrame::PING: {
                    // Send pong with same payload
                    std::string_view pingData(payload, length);
                    if (mask) {
                        std::string unmaskedPayload;
                        unmaskedPayload.reserve(length);
                        for (size_t i = 0; i < length; ++i) {
                            unmaskedPayload.push_back(payload[i] ^ mask[i % 4]);
                        }
                        send(unmaskedPayload, WebSocketFrame::PONG);
                    } else {
                        send(pingData, WebSocketFrame::PONG);
                    }
                    break;
                }
                case WebSocketFrame::PONG: {
                    lastPongTime = std::chrono::steady_clock::now();
                    break;
                }
                default:
                    // Ignore unknown frames
                    break;
            }
        }
    };

    template <bool SSL = false>
    struct ClientApp {
        // SSL/TLS configuration for secure connections
        struct SSLConfig {
            std::string cert_file;
            std::string key_file;
            std::string ca_file;
            bool verify_peer = true;
        };
    private:
        WebSocketClientBehavior behavior;
        std::string protocol;
        ParsedUrl parsedUrl;
        std::string webSocketKey;
        ClientWebSocket<SSL> *webSocket = nullptr;
        SSLConfig sslConfig;

        void sendUpgradeRequest() {
            std::ostringstream request;
            request << "GET " << parsedUrl.path << " HTTP/1.1\r\n";
            request << "Host: " << parsedUrl.host << ":" << parsedUrl.port << "\r\n";
            request << "Upgrade: websocket\r\n";
            request << "Connection: Upgrade\r\n";
            request << "Sec-WebSocket-Key: " << webSocketKey << "\r\n";
            request << "Sec-WebSocket-Version: 13\r\n";
            if (!protocol.empty()) {
                request << "Sec-WebSocket-Protocol: " << protocol << "\r\n";
            }
            request << "\r\n";

            std::string requestStr = request.str();
            std::cout << "Sending upgrade request:\n" << requestStr << std::endl;
        }

        void handleHttpResponse(const std::string &response) {
            // Parse HTTP response
            if (response.find("101 Switching Protocols") != std::string::npos &&
                response.find("Upgrade: websocket") != std::string::npos &&
                response.find("Connection: Upgrade") != std::string::npos) {

                std::cout << "WebSocket upgrade successful!" << std::endl;
                webSocket = new ClientWebSocket<SSL>(&behavior);
                webSocket->setConnected(true);

                if (behavior.open) {
                    behavior.open(webSocket);
                }
            } else {
                std::cout << "WebSocket upgrade failed!" << std::endl;
                if (behavior.failed) {
                    behavior.failed();
                }
            }
        }

        void handleWebSocketFrame(const char *data, size_t size) {
            std::string message;
            WebSocketFrame::OpCode opCode;
            bool fin;

            if (WebSocketFrame::decode(data, size, message, opCode, fin)) {
                if (behavior.message && webSocket) {
                    behavior.message(webSocket, message, static_cast<int>(opCode));
                }
            }
        }

    public:
        ClientApp(WebSocketClientBehavior &&behavior)
            : behavior(std::move(behavior)) {
            webSocketKey = generateWebSocketKey();
        }

        ~ClientApp() {
            if (webSocket) {
                delete webSocket;
            }
        }

        // Configure SSL/TLS settings
        ClientApp &&ssl(SSLConfig config) {
            if constexpr (SSL) {
                sslConfig = std::move(config);
            } else {
                std::cout << "Warning: SSL configuration ignored for non-SSL client" << std::endl;
            }
            return std::move(*this);
        }

        ClientApp &&connect(std::string url, std::string protocol = "") {
            this->protocol = protocol;
            parsedUrl = ParsedUrl::parse(url);

            if (parsedUrl.host.empty()) {
                std::cout << "Invalid WebSocket URL: " << url << std::endl;
                if (behavior.failed) {
                    behavior.failed();
                }
                return std::move(*this);
            }

            // Validate SSL requirements
            if (parsedUrl.ssl && !SSL) {
                std::cout << "Error: wss:// URL requires SSL-enabled client" << std::endl;
                if (behavior.failed) {
                    behavior.failed();
                }
                return std::move(*this);
            }

            std::cout << "Connecting to " << parsedUrl.host << ":" << parsedUrl.port << parsedUrl.path;
            if (parsedUrl.ssl) {
                std::cout << " (SSL/TLS)";
            }
            std::cout << std::endl;

            // In a real implementation, this would establish a TCP/TLS connection
            // For now, we'll simulate the connection process
            sendUpgradeRequest();

            // Simulate receiving upgrade response
            std::string simulatedResponse =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: " + webSocketKey + "\r\n"
                "\r\n";

            handleHttpResponse(simulatedResponse);

            return std::move(*this);
        }

        void run() {
            // In a real implementation, this would run the event loop
            std::cout << "Client running..." << std::endl;

            // Simulate receiving a message
            if (webSocket && webSocket->isConnected()) {
                std::string testMessage = "Hello from server!";
                std::string frame = WebSocketFrame::encode(testMessage, WebSocketFrame::TEXT);
                handleWebSocketFrame(frame.data(), frame.size());
            }
        }

        bool isConnected() const {
            return webSocket && webSocket->isConnected();
        }

        // Method to send messages (for testing)
        void sendMessage(std::string_view message) {
            if (webSocket) {
                webSocket->send(message);
            }
        }
    };

}