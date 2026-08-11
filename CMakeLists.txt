cmake_minimum_required(VERSION 3.15)
project(OrderBookReconstructor)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------
# 1. Fetch simdjson from GitHub (always, to get a proper library)
# ---------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG v3.9.0
)

# Disable tests and examples to speed up build
set(SIMDJSON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SIMDJSON_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(simdjson)

# ---------------------------------------------------------------------
# 2. Find Boost and OpenSSL
# ---------------------------------------------------------------------
find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------
# 3. Include Directories
# ---------------------------------------------------------------------
include_directories(include)
include_directories(${Boost_INCLUDE_DIRS})
# simdjson include is handled automatically by FetchContent

# ---------------------------------------------------------------------
# 4. Source Files
# ---------------------------------------------------------------------
set(SOURCES
    src/main.cpp
    src/orderbook/orderbook.cpp
    src/networking/rest_client.cpp
    src/networking/websocket_client.cpp
    src/engine/reconciler.cpp
    src/engine/validator.cpp
    src/engine/diff_history_buffer.cpp
    src/utils/price_parser.cpp
    
)


# ---------------------------------------------------------------------
# 5. Executable
# ---------------------------------------------------------------------
add_executable(orderbook_demo ${SOURCES})

# ---------------------------------------------------------------------
# 6. Link Libraries
# ---------------------------------------------------------------------
target_link_libraries(orderbook_demo
    ${Boost_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    simdjson::simdjson   # <-- This links the compiled library
    Threads::Threads
)

# ---------------------------------------------------------------------
# 7. Compiler Flags
# ---------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(orderbook_demo PRIVATE -O3)
endif()

target_compile_options(orderbook_demo PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------------
# 8. Windows-specific
# ---------------------------------------------------------------------
if(WIN32)
    target_link_libraries(orderbook_demo ws2_32 crypt32)
endif()

message(STATUS "==========================================")
message(STATUS "OrderBookReconstructor Configuration:")
message(STATUS "  Boost version: ${Boost_VERSION}")
message(STATUS "  OpenSSL version: ${OPENSSL_VERSION}")
message(STATUS "  simdjson built from source")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "==========================================")