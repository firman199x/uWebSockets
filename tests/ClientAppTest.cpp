// Test compilation of ClientApp
#include "../src/ClientApp.h"

int main() {
    // Test basic functionality
    uWS::WebSocketClientBehavior behavior;
    behavior.open = [](void *ws) { std::cout << "Open" << std::endl; };
    behavior.message = [](void *ws, std::string_view msg, int op) { std::cout << "Message: " << msg << std::endl; };
    behavior.close = [](void *ws, int code, std::string_view msg) { std::cout << "Close" << std::endl; };
    behavior.failed = []() { std::cout << "Failed" << std::endl; };

    uWS::ClientApp<false> app(std::move(behavior));
    return 0;
}