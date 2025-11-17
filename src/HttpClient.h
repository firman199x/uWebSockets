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
#include <mutex>
#include <atomic>
#include <future>
#include <chrono>
#include <unistd.h>
#include <vector>
#include <list>
#include <poll.h>
#ifdef LIBUS_USE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace uWS {

struct HttpReply {
    int status_code;
    std::string status_message;
    std::vector<std::pair<std::string_view, std::string_view>> headers;
    std::string body;
    std::function<void(HttpReply)> callback;
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
public:
    enum State { IDLE, CONNECTING, CONNECTED, REQUEST_SENT, READING, DONE };
private:
    std::function<void(HttpReply)> behavior;
    int socket_fd = -1;
    void *ssl = nullptr;
    bool connected = false;
    std::string host, port, path;
    bool use_ssl = false;
    std::string method = "GET";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int redirect_count = 0;
    std::string redirect_url;
    State state = IDLE;
    void *ssl_ctx = nullptr;

public:
    HttpClient(std::function<void(HttpReply)> b, void *ctx) : behavior(std::move(b)), ssl_ctx(ctx) {}

    ~HttpClient() {
        disconnect();
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
        if (host == "127.0.0.1") {
            server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        } else {
            struct addrinfo hints = {};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;
            struct addrinfo *result;
            int res = getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
            if (res != 0) {
                close(socket_fd);
                socket_fd = -1;
                return false;
            }
            if (!result) {
                close(socket_fd);
                socket_fd = -1;
                return false;
            }
            memcpy(&server_addr.sin_addr, &((sockaddr_in *)result->ai_addr)->sin_addr, sizeof(in_addr));
            freeaddrinfo(result);
        }

        state = CONNECTING;
        if (::connect(socket_fd, (sockaddr *)&server_addr, sizeof(server_addr)) < 0 && errno != EINPROGRESS) {
            state = DONE;
            close(socket_fd);
            socket_fd = -1;
            return false;
        }

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

    int get_fd() const { return socket_fd; }

    State get_state() const { return state; }

    void timeout() {
        state = DONE;
        disconnect();
        HttpReply reply;
        reply.status_code = -1;
        behavior(reply);
    }

    void sendRequest() {
        if (!connected) return;

        redirect_url.clear();

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
            state = DONE;
            disconnect();
            HttpReply reply;
            reply.status_code = -1;
            behavior(reply);
            return;
        }

        shutdown(socket_fd, SHUT_WR); // Close write side to indicate end of request
        state = REQUEST_SENT;
    }

    void process(bool can_read, bool can_write) {
        if (state == CONNECTING && can_write) {
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
                state = CONNECTED;
                connected = true;
                // SSL setup if needed
                if (use_ssl) {
#ifdef LIBUS_USE_OPENSSL
                    if (!ssl_ctx) {
                        state = DONE;
                        close(socket_fd);
                        socket_fd = -1;
                        HttpReply reply;
                        reply.status_code = -1;
                        behavior(reply);
                        return;
                    }
                    ssl = SSL_new(static_cast<SSL_CTX *>(ssl_ctx));
                    SSL_set_tlsext_host_name(static_cast<SSL *>(ssl), host.c_str());
                    SSL_set_fd(static_cast<SSL *>(ssl), socket_fd);
                    if (SSL_connect(static_cast<SSL *>(ssl)) != 1) {
                        SSL_free(static_cast<SSL *>(ssl));
                        ssl = nullptr;
                        state = DONE;
                        close(socket_fd);
                        socket_fd = -1;
                        HttpReply reply;
                        reply.status_code = -1;
                        behavior(reply);
                        return;
                    }
#endif
                }
                sendRequest();
            } else {
                state = DONE;
                close(socket_fd);
                socket_fd = -1;
                HttpReply reply;
                reply.status_code = -1;
                behavior(reply);
            }
        } else if (state == REQUEST_SENT && can_read) {
            readResponse();
        }
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
        int content_length = -1;
        size_t header_end = 0;
        bool is_redirect = false;

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
                                try {
                                  status_code = std::stoi(std::string(status_line.substr(space1 + 1, space2 - space1 - 1)));
                              } catch (...) {
                                  status_code = -1;
                              }
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
                            if (std::string_view(key) == "Content-Length") {
                                try {
                                    content_length = std::stoi(std::string(value));
                                } catch (...) {
                                    content_length = -1;
                                }
                            }
                        }
                        pos = line_end + 2;
                    }

                    headers_parsed = true;
                    // Remove headers from buffer
                    response_buffer.erase(0, header_end + 4);
                }
            }

            if (headers_parsed && !response_buffer.empty()) {
                // Accumulate body
            }
        }

        // End of response
        if (headers_parsed && (content_length == -1 || response_buffer.size() >= header_end + 4 + content_length)) {
            HttpReply reply;
            reply.status_code = status_code;
            reply.status_message = status_message;
            reply.headers = response_headers;
            size_t body_start = header_end + 4;
            size_t body_size = content_length != -1 ? content_length : response_buffer.size() - body_start;
            reply.body = response_buffer.substr(body_start, body_size);
            behavior(reply);
            state = DONE;
            disconnect();
        } else if (!headers_parsed && !response_buffer.empty()) {
            // Assume simple response without headers
            HttpReply reply;
            reply.status_code = 200;
            reply.status_message = "OK";
            reply.body = response_buffer;
            behavior(reply);
            state = DONE;
            disconnect();
        }
    }
};

struct PendingRequest {
    std::unique_ptr<HttpClient> client;
    std::chrono::time_point<std::chrono::high_resolution_clock> start_time;
    bool done = false;
};

static std::list<std::unique_ptr<PendingRequest>> pending_requests;
static std::mutex pending_mutex;

void processHttpRequests(int timeout_ms = 1000) {
    if (pending_requests.empty()) return;

    std::vector<pollfd> pollfds;
    std::vector<std::list<std::unique_ptr<PendingRequest>>::iterator> iters;
    for (auto it = pending_requests.begin(); it != pending_requests.end(); ++it) {
        auto &req = **it;
        int fd = req.client->get_fd();
        if (fd >= 0) {
            short events = 0;
            if (req.client->get_state() == HttpClient::CONNECTING) {
                events = POLLOUT;
            } else if (req.client->get_state() == HttpClient::REQUEST_SENT) {
                events = POLLIN;
            }
            if (events) {
                pollfds.push_back({fd, events, 0});
                iters.push_back(it);
            }
        }
    }

    if (pollfds.empty()) return;

    int ret = poll(pollfds.data(), pollfds.size(), timeout_ms);
    if (ret > 0) {
        for (size_t i = 0; i < pollfds.size(); ++i) {
            auto it = iters[i];
            auto &req = **it;
            bool can_read = pollfds[i].revents & POLLIN;
            bool can_write = pollfds[i].revents & POLLOUT;
            req.client->process(can_read, can_write);
        }
    }

    // Check timeouts and done
    std::vector<std::list<std::unique_ptr<PendingRequest>>::iterator> to_erase;
    for (auto it = pending_requests.begin(); it != pending_requests.end(); ++it) {
        auto &req = **it;
        if (req.client->get_state() == HttpClient::DONE) {
            req.done = true;
        } else {
            // Check for timeout
            auto now = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - req.start_time);
            if (elapsed.count() > 30) {
                req.client->timeout();
                req.done = true;
            }
        }

        if (req.done) {
            to_erase.push_back(it);
        }
    }
    for (auto it : to_erase) {
        pending_requests.erase(it);
    }
}

class HttpClientPool {
private:
    class HttpManager {
    public:
        std::thread eventThread;
        std::atomic<bool> running{false};
        std::atomic<int> refCount{0};
        std::atomic<int> request_count{0};
        void *ssl_ctx = nullptr;

        void start();
        void stop();
        void eventLoop();
    };

    static HttpManager manager;
    static std::mutex initMutex;

public:
    static std::future<HttpReply> HttpRequest(std::string_view method, std::string_view url,
                            std::function<void(HttpReply)> callback = nullptr,
                            std::string_view body = "{}", std::string_view content_type = "application/json",
                            std::string_view user_agent = "uWebSockets/1.0");
    static bool hasPendingRequests();
    static void wait();
    static void stop();
};

HttpClientPool::HttpManager HttpClientPool::manager{};
std::mutex HttpClientPool::initMutex{};

void HttpClientPool::HttpManager::start() {
    std::lock_guard<std::mutex> lock(initMutex);
    if (++refCount == 1 && !running) {
        running = true;
#ifdef LIBUS_USE_OPENSSL
        ssl_ctx = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_default_verify_paths(static_cast<SSL_CTX *>(ssl_ctx));
#endif
        eventThread = std::thread(&HttpClientPool::HttpManager::eventLoop, this);
    }
}

void HttpClientPool::HttpManager::stop() {
    std::lock_guard<std::mutex> lock(initMutex);
    if (--refCount == 0) {
        running = false;
        if (eventThread.joinable()) {
            eventThread.join();
        }
#ifdef LIBUS_USE_OPENSSL
        if (ssl_ctx) SSL_CTX_free(static_cast<SSL_CTX *>(ssl_ctx));
#endif
    }
}

void HttpClientPool::HttpManager::eventLoop() {
    while (running) {
        processHttpRequests(1000); // 1 second timeout
        {
            std::lock_guard<std::mutex> lock(pending_mutex);
            if (pending_requests.empty() && request_count == 0) {
                running = false;
            }
        }
    }
}

std::future<HttpReply> HttpClientPool::HttpRequest(std::string_view method, std::string_view url,
                                 std::function<void(HttpReply)> user_callback,
                                 std::string_view body, std::string_view content_type, std::string_view user_agent) {
    manager.start();

    auto promise_ptr = std::make_shared<std::promise<HttpReply>>();
    auto future = promise_ptr->get_future();

    ++manager.request_count;

    auto callback = [promise_ptr, user_callback, &request_count = manager.request_count](HttpReply reply) {
        reply.callback = user_callback;
        promise_ptr->set_value(std::move(reply));
        --request_count;
    };

    auto req = std::make_unique<PendingRequest>();
    req->client = std::make_unique<HttpClient>(callback, manager.ssl_ctx);
    req->client->setMethod(std::string(method));
    if (content_type != "{}") {
        req->client->addHeader("Content-Type", std::string(content_type));
    }
    if (user_agent != "{}") {
        req->client->addHeader("User-Agent", std::string(user_agent));
    }
    if (body != "{}") {
        req->client->setBody(std::string(body));
    }

    if (!req->client->connect(std::string(url))) {
        HttpReply reply;
        reply.status_code = -1;
        callback(reply);
        return future;
    }

    req->start_time = std::chrono::high_resolution_clock::now();

    std::lock_guard<std::mutex> lock(pending_mutex);
    pending_requests.push_back(std::move(req));

    return future;
}

bool HttpClientPool::hasPendingRequests() {
    std::lock_guard<std::mutex> lock(pending_mutex);
    return !pending_requests.empty();
}

void HttpClientPool::wait() {
    while (hasPendingRequests()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void HttpClientPool::stop() {
    manager.stop();
}

} // namespace uWS