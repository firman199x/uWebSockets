#pragma once

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
#include <cstdlib>
#include <errno.h>
#include <algorithm>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#ifdef LIBUS_USE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif
#ifdef __SSE2__
#include <emmintrin.h>
#endif
#ifdef __AVX2__
#include <immintrin.h>
#endif

namespace uWS {

struct WebSocketClientBehavior {
    MoveOnlyFunction<void(void *)> open;
    MoveOnlyFunction<void(void *, std::string_view, int)> message;
    MoveOnlyFunction<void(void *, int, std::string_view)> close;
    MoveOnlyFunction<void()> failed;
};

struct ParsedUrl {
    std::string host;
    std::string port;
    std::string path;
    bool ssl;

    static ParsedUrl parse(std::string_view url) {
        ParsedUrl result;
        if (url.size() >= 6 && url.substr(0, 6) == "wss://") {
            result.ssl = true;
        } else if (url.size() >= 5 && url.substr(0, 5) == "ws://") {
            result.ssl = false;
        } else {
            return result;
        }

        size_t start = result.ssl ? 6 : 5;
        size_t colonPos = url.find(':', start);
        size_t slashPos = url.find('/', start);

        if (colonPos != std::string_view::npos && (slashPos == std::string_view::npos || colonPos < slashPos)) {
            result.host = std::string(url.substr(start, colonPos - start));
            size_t portEnd = slashPos != std::string_view::npos ? slashPos : url.size();
            result.port = std::string(url.substr(colonPos + 1, portEnd - colonPos - 1));
        } else {
            size_t hostEnd = slashPos != std::string_view::npos ? slashPos : url.size();
            result.host = std::string(url.substr(start, hostEnd - start));
            result.port = result.ssl ? "443" : "80";
        }

        result.path = slashPos != std::string_view::npos ? std::string(url.substr(slashPos)) : "/";
        return result;
    }
};

inline std::string generateWebSocketKey() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    std::string key;
    for (int i = 0; i < 16; ++i) {
        key += static_cast<char>(dis(gen));
    }

    static const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    int val = 0, valb = -6;
    for (char c : key) {
        val = (val << 8) + static_cast<unsigned char>(c);
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

    static std::string encode(std::string_view message, OpCode opCode = TEXT, bool fin = true) {
        std::string frame;
        char header[14];
        size_t headerSize = 2;
        size_t length = message.size();

        header[0] = static_cast<char>((fin ? 0x80 : 0x00) | static_cast<unsigned char>(opCode));
        if (length <= 125) {
            header[1] = static_cast<char>(length | 0x80);
        } else if (length <= 65535) {
            header[1] = static_cast<char>(126 | 0x80);
            header[2] = static_cast<char>((length >> 8) & 0xFF);
            header[3] = static_cast<char>(length & 0xFF);
            headerSize = 4;
        } else {
            header[1] = static_cast<char>(127 | 0x80);
            for (int i = 0; i < 8; ++i) {
                header[2 + i] = static_cast<char>((length >> (56 - i * 8)) & 0xFF);
            }
            headerSize = 10;
        }

        unsigned char maskKey[4];
        generateMaskKey(maskKey);
        for (int i = 0; i < 4; ++i) {
            header[headerSize++] = static_cast<char>(maskKey[i]);
        }

        frame.resize(getEncodedSize(length));
        memcpy(frame.data(), header, headerSize);
        memcpy(frame.data() + headerSize, message.data(), length);
        maskData(frame.data() + headerSize, length, maskKey);

        return frame;
    }

    static size_t decode(const char *data, size_t size, std::string &outMessage, OpCode &outOpCode, bool &outFin) {
        if (size < 2) return 0;

        unsigned char firstByte = static_cast<unsigned char>(data[0]);
        outFin = (firstByte & 0x80) != 0;
        outOpCode = static_cast<OpCode>(firstByte & 0x0F);

        unsigned char secondByte = static_cast<unsigned char>(data[1]);
        bool masked = (secondByte & 0x80) != 0;
        size_t length = secondByte & 0x7F;

        size_t headerSize = 2;
        if (length == 126) {
            if (size < 4) return 0;
            length = (static_cast<unsigned char>(data[2]) << 8) | static_cast<unsigned char>(data[3]);
            headerSize = 4;
        } else if (length == 127) {
            if (size < 10) return 0;
            length = 0;
            for (int i = 0; i < 8; ++i) {
                length = (length << 8) | static_cast<unsigned char>(data[2 + i]);
            }
            headerSize = 10;
        }

        if (masked) {
            if (size < headerSize + 4 + length) return 0;
            const unsigned char *mask = reinterpret_cast<const unsigned char *>(data + headerSize);
            headerSize += 4;
            outMessage.resize(length);
            memcpy(outMessage.data(), data + headerSize, length);
            maskData(outMessage.data(), length, mask);
        } else {
            if (size < headerSize + length) return 0;
            outMessage.assign(data + headerSize, length);
        }

        return headerSize + length;
    }

    static size_t getEncodedSize(size_t payloadSize) {
        size_t headerSize = 2;
        if (payloadSize <= 125) {
        } else if (payloadSize <= 65535) {
            headerSize += 2;
        } else {
            headerSize += 8;
        }
        headerSize += 4; // mask
        return headerSize + payloadSize;
    }

private:
    static void maskData(char *data, size_t length, const unsigned char mask[4]) {
        size_t i = 0;
#ifdef __AVX2__
        __m256i avxMaskVec = _mm256_set_epi8(
            static_cast<char>(mask[3]), static_cast<char>(mask[2]), static_cast<char>(mask[1]), static_cast<char>(mask[0]),
            static_cast<char>(mask[3]), static_cast<char>(mask[2]), static_cast<char>(mask[1]), static_cast<char>(mask[0]),
            static_cast<char>(mask[3]), static_cast<char>(mask[2]), static_cast<char>(mask[1]), static_cast<char>(mask[0]),
            static_cast<char>(mask[3]), static_cast<char>(mask[2]), static_cast<char>(mask[1]), static_cast<char>(mask[0]),
            static_cast<char>(mask[3]), static_cast<char>(mask[2]), static_cast<char>(mask[1]), static_cast<char>(mask[0]),
            static_cast<char>(mask[3]), static_cast<char>(mask[2]), static_cast<char>(mask[1]), static_cast<char>(mask[0]),
            static_cast<char>(mask[3]), static_cast<char>(mask[2]), static_cast<char>(mask[1]), static_cast<char>(mask[0]),
            static_cast<char>(mask[3]), static_cast<char>(mask[2]), static_cast<char>(mask[1]), static_cast<char>(mask[0])
        );
        for (; i + 31 < length; i += 32) {
            __m256i dataVec = _mm256_loadu_si256(reinterpret_cast<__m256i*>(data + i));
            __m256i masked = _mm256_xor_si256(dataVec, avxMaskVec);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(data + i), masked);
        }
#endif
#ifdef __SSE2__
        __m128i sseMaskVec = _mm_set_epi8(
            static_cast<char>(mask[3]), static_cast<char>(mask[2]), static_cast<char>(mask[1]), static_cast<char>(mask[0]),
            static_cast<char>(mask[3]), static_cast<char>(mask[2]), static_cast<char>(mask[1]), static_cast<char>(mask[0]),
            static_cast<char>(mask[3]), static_cast<char>(mask[2]), static_cast<char>(mask[1]), static_cast<char>(mask[0]),
            static_cast<char>(mask[3]), static_cast<char>(mask[2]), static_cast<char>(mask[1]), static_cast<char>(mask[0])
        );
        for (; i + 15 < length; i += 16) {
            __m128i dataVec = _mm_loadu_si128(reinterpret_cast<__m128i*>(data + i));
            __m128i masked = _mm_xor_si128(dataVec, sseMaskVec);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(data + i), masked);
        }
#endif
        for (; i < length; ++i) {
            data[i] ^= static_cast<char>(mask[i % 4]);
        }
    }

    static void generateMaskKey(unsigned char maskKey[4]) {
        static thread_local std::mt19937 gen(std::random_device{}());
        std::uniform_int_distribution<unsigned char> dis(0, 255);
        for (int i = 0; i < 4; ++i) {
            maskKey[i] = dis(gen);
        }
    }
};

class ClientWebSocket {
private:
    WebSocketClientBehavior *behavior;
    int socket_fd;
#ifdef LIBUS_USE_OPENSSL
    SSL *ssl;
#endif
    bool connected = false;
    std::vector<char> sendBuffer;
    std::vector<char> receiveBuffer;
    size_t readOffset = 0;

public:
    ClientWebSocket(WebSocketClientBehavior *b, int fd, void * /*s*/) : behavior(b), socket_fd(fd)
#ifdef LIBUS_USE_OPENSSL
    , ssl(static_cast<SSL*>(s))
#endif
    {}

    void send(std::string_view message, WebSocketFrame::OpCode opCode = WebSocketFrame::TEXT) {
        std::string frame = WebSocketFrame::encode(message, opCode);
        sendBuffer.insert(sendBuffer.end(), frame.begin(), frame.end());
        flushSendBuffer();
    }

    void processData(char *data, int length) {
        receiveBuffer.insert(receiveBuffer.end(), data, data + length);
        processReceiveBuffer();
    }

    void setConnected(bool c) { connected = c; }
    bool isConnected() const { return connected; }

private:
    void flushSendBuffer() {
        if (!sendBuffer.empty() && socket_fd >= 0) {
#ifdef LIBUS_USE_OPENSSL
            if (ssl) {
                ssize_t written = SSL_write(static_cast<SSL*>(ssl), sendBuffer.data(), static_cast<int>(sendBuffer.size()));
                if (written > 0) {
                    sendBuffer.erase(sendBuffer.begin(), sendBuffer.begin() + written);
                } else if (written < 0) {
                    int ssl_err = SSL_get_error(static_cast<SSL*>(ssl), static_cast<int>(written));
                    if (ssl_err == SSL_ERROR_WANT_WRITE) {
                        // Would block, try again later
                    }
                    // Handle other errors if needed
                }
            } else {
#endif
                ssize_t written = write(socket_fd, sendBuffer.data(), sendBuffer.size());
                if (written > 0) {
                    sendBuffer.erase(sendBuffer.begin(), sendBuffer.begin() + written);
                } else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    // Would block, try again later
                }
                // Handle other errors if needed
#ifdef LIBUS_USE_OPENSSL
            }
#endif
        }
    }

    void processReceiveBuffer() {
        size_t offset = readOffset;
        while (offset < receiveBuffer.size()) {
            std::string message;
            WebSocketFrame::OpCode opCode;
            bool fin;
            size_t consumed = WebSocketFrame::decode(receiveBuffer.data() + offset, receiveBuffer.size() - offset, message, opCode, fin);
            if (consumed) {
                offset += consumed;
                if (behavior && behavior->message) {
                    behavior->message(this, message, (int)opCode);
                }
            } else {
                break;
            }
        }
        readOffset = offset;
        if (readOffset > receiveBuffer.size() / 2) {
            receiveBuffer.erase(receiveBuffer.begin(), receiveBuffer.begin() + static_cast<ptrdiff_t>(readOffset));
            readOffset = 0;
        }
    }
};

class ClientApp {
private:
    WebSocketClientBehavior behavior;
    ClientWebSocket *ws = nullptr;
    int socket_fd = -1;
    void *ssl = nullptr;
#ifdef LIBUS_USE_OPENSSL
    SSL_CTX *ssl_ctx = nullptr;
#endif
    bool connected = false;
    std::string host, port, path;
    bool use_ssl = false;

public:
    ClientApp(WebSocketClientBehavior &&b) : behavior(std::move(b)) {}

    ~ClientApp() {
        disconnect();
#ifdef LIBUS_USE_OPENSSL
        if (ssl_ctx) SSL_CTX_free(ssl_ctx);
#endif
    }

    bool connect(std::string url) {
        ParsedUrl parsed = ParsedUrl::parse(url);
        if (parsed.host.empty()) return false;

        host = parsed.host;
        port = parsed.port;
        path = parsed.path;
        use_ssl = parsed.ssl;

        // Create socket
        socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd < 0) return false;

        // Set non-blocking
        fcntl(socket_fd, F_SETFL, O_NONBLOCK);

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(static_cast<uint16_t>(std::stoi(port)));
        inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr);

        if (::connect(socket_fd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0 && errno != EINPROGRESS) {
            close(socket_fd);
            socket_fd = -1;
            return false;
        }

        // Wait for connection
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(socket_fd, &writefds);
        struct timeval tv = {5, 0}; // 5 second timeout
        if (select(socket_fd + 1, nullptr, &writefds, nullptr, &tv) <= 0) {
            close(socket_fd);
            socket_fd = -1;
            return false;
        }

        int error;
        socklen_t len = sizeof(error);
        getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &error, &len);
        if (error) {
            close(socket_fd);
            socket_fd = -1;
            return false;
        }

        // Keep non-blocking for async I/O

        // SSL setup if needed
        if (use_ssl) {
#ifdef LIBUS_USE_OPENSSL
            SSL_library_init();
            ssl_ctx = SSL_CTX_new(TLS_client_method());
            SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, nullptr);
            if (!ssl_ctx) {
                close(socket_fd);
                socket_fd = -1;
                return false;
            }
            ssl = SSL_new(ssl_ctx);
            SSL_set_fd(static_cast<SSL*>(ssl), socket_fd);
            if (SSL_connect(static_cast<SSL*>(ssl)) != 1) {
                SSL_free(static_cast<SSL*>(ssl));
                ssl = nullptr;
                SSL_CTX_free(ssl_ctx);
                ssl_ctx = nullptr;
                close(socket_fd);
                socket_fd = -1;
                return false;
            }
#else
            // SSL not supported
            close(socket_fd);
            socket_fd = -1;
            return false;
#endif
        }

        // Perform WebSocket handshake
        std::string key = generateWebSocketKey();
        std::string handshake = "GET " + path + " HTTP/1.1\r\n"
            "Host: " + host + ":" + port + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + key + "\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";

        ssize_t sent;
#ifdef LIBUS_USE_OPENSSL
        if (use_ssl) {
            sent = SSL_write(static_cast<SSL*>(ssl), handshake.data(), handshake.size());
        } else
#endif
        {
            sent = write(socket_fd, handshake.data(), handshake.size());
        }
        if (sent != static_cast<ssize_t>(handshake.size())) {
            disconnect();
            return false;
        }

        // Read response
        char response_buffer[4096];
        size_t response_size = 0;
        ssize_t n;
        // Wait for response
        struct pollfd pfd = {socket_fd, POLLIN, 0};
        int timeout_ms = 5000; // 5 second timeout
        if (poll(&pfd, 1, timeout_ms) <= 0) {
            disconnect();
            return false;
        }
        while (response_size < sizeof(response_buffer)) {
#ifdef LIBUS_USE_OPENSSL
            if (use_ssl) {
                n = SSL_read(static_cast<SSL*>(ssl), response_buffer + response_size, sizeof(response_buffer) - response_size);
                if (n < 0) {
                    int ssl_err = SSL_get_error(static_cast<SSL*>(ssl), static_cast<int>(n));
                    if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                        continue; // would block, try again
                    }
                    break;
                }
            } else
#endif
            {
                n = read(socket_fd, response_buffer + response_size, sizeof(response_buffer) - response_size);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        continue; // would block, try again
                    }
                    break;
                }
            }
            if (n == 0) break;
            response_size += n;
            // Check for end of headers
            if (response_size >= 4 && memcmp(response_buffer + response_size - 4, "\r\n\r\n", 4) == 0) break;
        }

        // Parse status line
        std::string_view response_view(response_buffer, response_size);
        size_t status_end = response_view.find("\r\n");
        if (status_end == std::string_view::npos) {
            disconnect();
            return false;
        }
        std::string_view status_line = response_view.substr(0, status_end);
        if (status_line.find("101") == std::string_view::npos) {
            disconnect();
            return false;
        }

        // Create WebSocket
        ws = new ClientWebSocket(&behavior, socket_fd, ssl);
        ws->setConnected(true);
        connected = true;

        if (behavior.open) {
            behavior.open(ws);
        }

        return true;
    }

    void disconnect() {
        if (ws) {
            ws->setConnected(false);
            delete ws;
            ws = nullptr;
        }
#ifdef LIBUS_USE_OPENSSL
        if (ssl) {
            SSL_shutdown(static_cast<SSL*>(ssl));
            SSL_free(static_cast<SSL*>(ssl));
            ssl = nullptr;
        }
#endif
        if (socket_fd >= 0) {
            close(socket_fd);
            socket_fd = -1;
        }
        connected = false;
    }

    bool isConnected() const { return connected; }

    void sendMessage(std::string message) {
        if (ws && connected) {
            ws->send(message);
        }
    }

    void run() {
        if (!connected) return;

        char buffer[4096];
        ssize_t n;
#ifdef LIBUS_USE_OPENSSL
        if (ssl) {
            n = SSL_read(static_cast<SSL*>(ssl), buffer, sizeof(buffer));
            if (n < 0) {
                int ssl_err = SSL_get_error(static_cast<SSL*>(ssl), static_cast<int>(n));
                if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                    return; // would block, try again later
                }
                // Handle other SSL errors if needed
                return;
            }
        } else
#endif
        {
            n = read(socket_fd, buffer, sizeof(buffer));
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return; // no data available, try again later
                }
                // Handle other errors if needed
                return;
            }
        }
        if (n > 0) {
            ws->processData(buffer, static_cast<int>(n));
        } else if (n == 0) {
            // Connection closed
            if (behavior.close) {
                behavior.close(ws, 1000, "Connection closed");
            }
            disconnect();
        }
    }
};

}