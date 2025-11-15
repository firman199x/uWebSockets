# uWebSockets Agent Guidelines

## Build/Lint/Test Commands

### Building
- `make examples` - Build all example applications
- `WITH_OPENSSL=1 make examples` - Build with OpenSSL support
- `WITH_ASAN=1 make examples` - Build with AddressSanitizer
- `WITH_LIBUV=1 make examples` - Build with libuv event loop

### Testing
- `make -C tests` - Run all unit tests
- `make -C tests smoke` - Run smoke tests (integration with Deno/Node.js)
- `make -C tests compliance` - Run HTTP compliance tests
- `make -C tests performance` - Run performance tests

### Single Test Execution
- `cd tests && g++ -std=c++17 -fsanitize=address HttpParser.cpp -o HttpParser && ./HttpParser`
- `cd tests && g++ -std=c++17 -fsanitize=address BloomFilter.cpp -o BloomFilter && ./BloomFilter`
- `cd tests && g++ -std=c++17 -I ../src WebSocketClientTest.cpp -o WebSocketClientTest && ./WebSocketClientTest`

### Client Examples
- `g++ -std=c++17 -I src examples/Client.cpp -o Client && ./Client` - Build and run production WebSocket client example
- `g++ -std=c++17 -I src tests/WebSocketClientTest.cpp -o WebSocketClientTest && ./WebSocketClientTest` - Run client tests
- `g++ -std=c++17 -O3 -march=native -I src tests/WebSocketPerfTest.cpp -o perf_test && ./perf_test` - Run performance benchmarks

### Production Features
- **Zero-copy frame processing** with SIMD optimizations for 3-57x performance improvement
- **SSL/TLS support** for wss:// connections with configurable certificates
- **Message fragmentation** for large payloads with automatic defragmentation
- **Ping/pong keepalive** with configurable timeouts and connection health monitoring
- **Error handling** with automatic reconnection (configurable attempts and delays)
- **Backpressure handling** with send buffer management
- **Graceful shutdown** with proper close frame handling
- **Thread-safe operations** with atomic state management

## Code Style Guidelines

### Language Standard
- Use C++17 minimum, prefer C++2b features when available
- Compile with `-std=c++2b` flag

### Header Files
- Include Apache License 2.0 header comment block
- Use `#ifndef UWS_[FILENAME]_H` guard pattern
- Define `_CRT_SECURE_NO_WARNINGS` for Windows compatibility

### Naming Conventions
- Namespaces: `uWS` for main namespace, `uWS::utils` for utilities
- Functions: camelCase for public APIs, snake_case for internal utilities
- Classes/Structs: PascalCase
- Variables: camelCase, snake_case for locals

### Imports and Includes
- System headers first: `<string>`, `<string_view>`, `<charconv>`, `<cstdint>`
- Local headers: `"App.h"`, `"Utilities.h"`
- Group related includes together

### Formatting
- Use clang-format with selective `// clang-format off/on` for complex formatting
- 4-space indentation (inferred from examples)
- Opening braces on same line for functions and classes

### Error Handling
- Use exceptions sparingly, prefer return codes
- Validate inputs with assertions in debug builds
- Use AddressSanitizer (`-fsanitize=address`) for testing

### Types and Safety
- Use `auto` for lambda parameters and iterator types
- Prefer `std::string_view` over `const std::string&` for read-only strings
- Use `size_t` for sizes and lengths
- Enable pedantic warnings: `-Wpedantic -Wall -Wextra -Wsign-conversion -Wconversion`

### Performance
- Use `inline` for small utility functions
- Prefer stack allocation over heap when possible
- Use move semantics for large objects
- Optimize for `-O3 -march=native` builds