#pragma once

#include "MoveOnlyFunction.h"
#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <netdb.h>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>
#ifdef LIBUS_USE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif
#ifdef __SSE2__
#include <emmintrin.h>
#endif
#ifdef __AVX2__
#include <immintrin.h>
#endif

namespace uWS {

struct HttpClientBehavior {
    MoveOnlyFunction<void(void *, int, std::string_view, std::vector<std::pair<std::string_view, std::string_view>>)> response;
    MoveOnlyFunction<void(void *, std::string_view, bool)> data;
    MoveOnlyFunction<void()> failed;
};

struct HttpParsedUrl {
    std::string host;
    std::string port;
    std::string path;
    bool ssl;

    static HttpParsedUrl parse(std::string_view url) {
        HttpParsedUrl result;
        if (url.size() >= 8 && url.substr(0, 8) == "https://") {
            result.ssl = true;
        } else if (url.size() >= 7 && url.substr(0, 7) == "http://") {
            result.ssl = false;
        } else {
            return result;
        }

        size_t start = result.ssl ? 8 : 7;
        size_t colonPos = url.find(':', start);
        size_t slashPos = url.find('/', start);

        if (colonPos != std::string_view::npos &&
            (slashPos == std::string_view::npos || colonPos < slashPos)) {
            result.host = std::string(url.substr(start, colonPos - start));
            size_t portEnd =
                slashPos != std::string_view::npos ? slashPos : url.size();
            result.port =
                std::string(url.substr(colonPos + 1, portEnd - colonPos - 1));
        } else {
            size_t hostEnd =
                slashPos != std::string_view::npos ? slashPos : url.size();
            result.host = std::string(url.substr(start, hostEnd - start));
            result.port = result.ssl ? "443" : "80";
        }

        result.path = slashPos != std::string_view::npos
                          ? std::string(url.substr(slashPos))
                          : "/";
        return result;
    }
};

class HttpClient {
private:
    HttpClientBehavior behavior;
    int socket_fd = -1;
    void *ssl = nullptr;
#ifdef LIBUS_USE_OPENSSL
    SSL_CTX *ssl_ctx = nullptr;
#endif
    bool connected = false;
    std::string host, port, path;
    bool use_ssl = false;
    std::string method = "GET";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

public:
    HttpClient(HttpClientBehavior &&b) : behavior(std::move(b)) {}

    ~HttpClient() {
        disconnect();
#ifdef LIBUS_USE_OPENSSL
        if (ssl_ctx)
            SSL_CTX_free(ssl_ctx);
#endif
    }

    void setMethod(std::string m) { method = std::move(m); }
    void addHeader(std::string key, std::string value) { headers.emplace_back(std::move(key), std::move(value)); }
    void setBody(std::string b) { body = std::move(b); }

    bool connect(std::string url) {
        HttpParsedUrl parsed = HttpParsedUrl::parse(url);
        if (parsed.host.empty())
            return false;

        host = parsed.host;
        port = parsed.port;
        path = parsed.path;
        use_ssl = parsed.ssl;

        // Create socket
        socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd < 0)
            return false;

        // Set non-blocking
        fcntl(socket_fd, F_SETFL, O_NONBLOCK);

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(static_cast<uint16_t>(std::stoi(port)));

        // Resolve hostname
        struct hostent *he = gethostbyname(host.c_str());
        if (!he) return false;
        memcpy(&server_addr.sin_addr, he->h_addr, he->h_length);

        if (::connect(socket_fd, (sockaddr *)&server_addr, sizeof(server_addr)) <
                0 &&
            errno != EINPROGRESS) {
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
            SSL_set_fd(static_cast<SSL *>(ssl), socket_fd);
            if (SSL_connect(static_cast<SSL *>(ssl)) != 1) {
                SSL_free(static_cast<SSL *>(ssl));
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

        connected = true;
        return true;
    }

    void disconnect() {
#ifdef LIBUS_USE_OPENSSL
        if (ssl) {
            SSL_shutdown(static_cast<SSL *>(ssl));
            SSL_free(static_cast<SSL *>(ssl));
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

    void sendRequest() {
        if (!connected) return;

        std::string request = method + " " + path + " HTTP/1.1\r\n";
        request += "Host: " + host + ":" + port + "\r\n";
        for (auto &[key, value] : headers) {
            request += key + ": " + value + "\r\n";
        }
        if (!body.empty()) {
            request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        request += "\r\n";
        if (!body.empty()) {
            request += body;
        }

        ssize_t sent;
#ifdef LIBUS_USE_OPENSSL
        if (use_ssl) {
            sent = SSL_write(static_cast<SSL *>(ssl), request.data(), request.size());
        } else
#endif
        {
            sent = write(socket_fd, request.data(), request.size());
        }
        if (sent != static_cast<ssize_t>(request.size())) {
            disconnect();
            if (behavior.failed) behavior.failed();
            return;
        }

        // Now read response
        readResponse();
    }

private:
    void readResponse() {
        std::string response_buffer;
        char buffer[4096];
        ssize_t n;
        bool headers_parsed = false;
        int status_code = 0;
        std::string status_message;
        std::vector<std::pair<std::string_view, std::string_view>> response_headers;

        while (true) {
#ifdef LIBUS_USE_OPENSSL
            if (use_ssl) {
                n = SSL_read(static_cast<SSL *>(ssl), buffer, sizeof(buffer));
                if (n < 0) {
                    int ssl_err = SSL_get_error(static_cast<SSL *>(ssl), static_cast<int>(n));
                    if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                        continue;
                    }
                    break;
                }
            } else
#endif
            {
                n = read(socket_fd, buffer, sizeof(buffer));
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        continue;
                    }
                    break;
                }
            }
            if (n == 0) break;
            response_buffer.append(buffer, n);

            if (!headers_parsed) {
                size_t header_end = response_buffer.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    std::string_view headers_part = response_buffer.substr(0, header_end);
                    size_t status_end = headers_part.find("\r\n");
                    if (status_end != std::string_view::npos) {
                        std::string_view status_line = headers_part.substr(0, status_end);
                        // Parse status line: HTTP/1.1 200 OK
                        size_t space1 = status_line.find(' ');
                        if (space1 != std::string_view::npos) {
                            size_t space2 = status_line.find(' ', space1 + 1);
                            if (space2 != std::string_view::npos) {
                                status_code = std::stoi(std::string(status_line.substr(space1 + 1, space2 - space1 - 1)));
                                status_message = std::string(status_line.substr(space2 + 1));
                            }
                        }
                    }

                    // Parse headers
                    size_t pos = status_end + 2;
                    while (pos < header_end) {
                        size_t line_end = headers_part.find("\r\n", pos);
                        if (line_end == std::string_view::npos) break;
                        std::string_view line = headers_part.substr(pos, line_end - pos);
                        size_t colon = line.find(':');
                        if (colon != std::string_view::npos) {
                            std::string_view key = line.substr(0, colon);
                            std::string_view value = line.substr(colon + 1);
                            // Trim whitespace
                            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
                            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
                            response_headers.emplace_back(key, value);
                        }
                        pos = line_end + 2;
                    }

                    headers_parsed = true;
                    if (behavior.response) {
                        behavior.response(this, status_code, status_message, response_headers);
                    }

                    // Remove headers from buffer
                    response_buffer.erase(0, header_end + 4);
                }
            }

            if (headers_parsed && !response_buffer.empty()) {
                if (behavior.data) {
                    behavior.data(this, response_buffer, false);
                }
                response_buffer.clear();
            }
        }

        // End of data
        if (behavior.data) {
            behavior.data(this, {}, true);
        }
    }
};

} // namespace uWS