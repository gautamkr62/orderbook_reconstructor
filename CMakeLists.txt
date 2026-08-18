cmake_minimum_required(VERSION 3.15)
project(OrderBookReconstructor)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------
# 1. Fetch dependencies
# ---------------------------------------------------------------------
include(FetchContent)

# ---- Google Test ----
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG release-1.12.1
)

# ---- simdjson ----
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG v3.9.0
)

# Disable simdjson tests/examples to speed up build
set(SIMDJSON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SIMDJSON_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# Make both available
FetchContent_MakeAvailable(googletest simdjson)

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
    src/engine/apply_delta.cpp
    src/engine/comparebooks.cpp
    src/utils/price_parser.cpp
)


# # In CMakeLists.txt, after project() and before add_executable()
# if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
#     set(TSAN_FLAGS "-fsanitize=thread -g -O1 -fno-omit-frame-pointer")
#     set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${TSAN_FLAGS}")
#     set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
# endif()


# ---------------------------------------------------------------------
# ThreadSanitizer build type
# ---------------------------------------------------------------------
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS_TSAN "-fsanitize=thread -g -O1 -fno-omit-frame-pointer -Wno-tsan")
    set(CMAKE_EXE_LINKER_FLAGS_TSAN "-fsanitize=thread")
endif()

# ---------------------------------------------------------------------
# 5. Main Executable
# ---------------------------------------------------------------------
add_executable(orderbook_demo ${SOURCES})

target_link_libraries(orderbook_demo
    ${Boost_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    simdjson::simdjson
    Threads::Threads
)

# ---------------------------------------------------------------------
# 6. Compiler Flags
# ---------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(orderbook_demo PRIVATE -O3)
endif()
target_compile_options(orderbook_demo PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------------
# 7. Windows-specific
# ---------------------------------------------------------------------
if(WIN32)
    target_link_libraries(orderbook_demo ws2_32 crypt32)
endif()

# ---------------------------------------------------------------------
# 8. Unit Tests
# ---------------------------------------------------------------------
# enable_testing()

# # ---- test_price_parser ----
# add_executable(test_price_parser
#     tests/test_price_parser.cpp
#     src/utils/price_parser.cpp
# )

# target_include_directories(test_price_parser PRIVATE
#     ${PROJECT_SOURCE_DIR}/include
# )

# target_link_libraries(test_price_parser
#     GTest::gtest_main
# )

# # ---- Discover tests with CTest ----
# include(GoogleTest)
# gtest_discover_tests(test_price_parser)

# # ------ test side_book --------
# add_executable(test_sidebook
#     tests/test_sidebook.cpp
#     src/orderbook/orderbook.cpp
#     include/orderbook/side_book.hpp
#     src/utils/price_parser.cpp
# )

# target_include_directories(test_sidebook PRIVATE
#     ${PROJECT_SOURCE_DIR}/include
# )

# target_link_libraries(test_sidebook
#     GTest::gtest_main
# )

# gtest_discover_tests(test_sidebook)

# # ---- test_diffhistory ----
# add_executable(test_diffhistory
#     tests/test_diffhistory.cpp
#     src/engine/diff_history_buffer.cpp
# )

# target_include_directories(test_diffhistory PRIVATE
#     ${PROJECT_SOURCE_DIR}/include
# )

# target_link_libraries(test_diffhistory
#     GTest::gtest_main
# )

# gtest_discover_tests(test_diffhistory)

# # ---- test_orderbook ----
# add_executable(test_orderbook
#     tests/test_orderbook.cpp
#     src/orderbook/orderbook.cpp
# )

# target_include_directories(test_orderbook PRIVATE
#     ${PROJECT_SOURCE_DIR}/include
# )

# target_link_libraries(test_orderbook
#     GTest::gtest_main
# )

# gtest_discover_tests(test_orderbook)

# # ---- test_applydelta ----
# add_executable(test_applydelta
#     tests/test_applydelta.cpp
#     src/orderbook/orderbook.cpp
#     src/engine/apply_delta.cpp          # <-- only these two
# )

# target_include_directories(test_applydelta PRIVATE
#     ${PROJECT_SOURCE_DIR}/include
# )

# target_link_libraries(test_applydelta
#     GTest::gtest_main
# )

# gtest_discover_tests(test_applydelta)

# # ---- test_comparebooks ----
# add_executable(test_comparebooks
#     tests/test_comparebooks.cpp
#     src/engine/comparebooks.cpp          # the implementation
#     src/orderbook/orderbook.cpp          # OrderBook is needed to build the book
# )

# target_include_directories(test_comparebooks PRIVATE
#     ${PROJECT_SOURCE_DIR}/include
# )

# target_link_libraries(test_comparebooks
#     GTest::gtest_main
# )

# gtest_discover_tests(test_comparebooks)


# ---------------------------------------------------------------------
# 9. Thread Sanitiser
# ---------------------------------------------------------------------





# ---------------------------------------------------------------------
# 10. Configuration Summary
# ---------------------------------------------------------------------
message(STATUS "==========================================")
message(STATUS "OrderBookReconstructor Configuration:")
message(STATUS "  Boost version: ${Boost_VERSION}")
message(STATUS "  OpenSSL version: ${OPENSSL_VERSION}")
message(STATUS "  simdjson built from source")
message(STATUS "  Google Test included")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "==========================================")