/* We simply call the root header file "App.h", giving you uWS::App and uWS::SSLApp */
#include "App.h"
#include <chrono>
#include <iomanip>

/* This is a simple WebSocket echo server example.
 * You may compile it with "WITH_OPENSSL=1 make" or with "make" */

int main() {
    /* ws->getUserData returns one of these */
    struct PerSocketData {
        /* Fill with user data */
    };

    /* Keep in mind that uWS::SSLApp({options}) is the same as uWS::App() when compiled without SSL support.
     * You may swap to using uWS:App() if you don't need SSL */
    uWS::App app;
    app.ws<PerSocketData>("/*", {
        /* Settings */
        .compression = uWS::CompressOptions(uWS::DEDICATED_COMPRESSOR | uWS::DEDICATED_DECOMPRESSOR),
        .maxPayloadLength = 100 * 1024 * 1024,
        .idleTimeout = 16,
        .maxBackpressure = 100 * 1024 * 1024,
        .closeOnBackpressureLimit = false,
        .resetIdleTimeoutOnSend = false,
        .sendPingsAutomatically = true,
        /* Handlers */
        .upgrade = nullptr,
        .open = [](auto *ws) {
            /* Open event here, you may access ws->getUserData() now */
            std::cout << "New connection" << std::endl;
        },
        .message = [](auto *ws, std::string_view message, uWS::OpCode opCode) {
            /* Echo the message back */
            auto recvTime = std::chrono::system_clock::now();
            auto recv_t = std::chrono::system_clock::to_time_t(recvTime);
            std::cout << "📨 Server received at " << std::put_time(std::gmtime(&recv_t), "%F %T") << ": " << message << std::endl;
            ws->send(message, opCode);
            auto sendTime = std::chrono::system_clock::now();
            auto send_t = std::chrono::system_clock::to_time_t(sendTime);
            std::cout << "📤 Server sent reply at " << std::put_time(std::gmtime(&send_t), "%F %T") << std::endl;
        },
        .close = [](auto *ws, int code, std::string_view message) {
            /* You may access ws->getUserData() here */
        }
    }).listen(9001, [](auto *listen_socket) {
        if (listen_socket) {
            std::cout << "Listening on port " << 9001 << std::endl;
        }
    }).run();
}
