cmake_minimum_required(VERSION 3.16)
project(mnet VERSION 0.2.5 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The same warning levels as the Makefile: everything on, warnings are errors.
if(MSVC)
    add_compile_options(/W4 /WX)
    # MSVC warns (C4996) about standard C functions it considers "unsafe POSIX
    # names" — sscanf, strcpy, strdup, etc. These are false positives for
    # portable C code; suppress them so /WX does not escalate them to errors.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

if(NOT WIN32)
    add_compile_definitions(_POSIX_C_SOURCE=200112L)
endif()
if(APPLE)
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()

# The worker pool needs threads on POSIX.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Optional hardening/sanitizer build: -DMNET_SANITIZE=ON
option(MNET_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(MNET_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# Source files
set(MNET_SOURCES
    src/mnet.c
    src/mnet_app.c
    src/mnet_response.c
    src/mnet_request.c
    src/mnet_router.c
)

# Static library
add_library(mnet_static STATIC ${MNET_SOURCES})
target_include_directories(mnet_static PUBLIC include PRIVATE include/mnet)
set_target_properties(mnet_static PROPERTIES OUTPUT_NAME mnet)

# Shared library
add_library(mnet_shared SHARED ${MNET_SOURCES})
target_include_directories(mnet_shared PUBLIC include PRIVATE include/mnet)
set_target_properties(mnet_shared PROPERTIES
    OUTPUT_NAME mnet
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)

# Threads are part of the library's implementation, so consumers must link them.
target_link_libraries(mnet_static PUBLIC Threads::Threads)
target_link_libraries(mnet_shared PRIVATE Threads::Threads)

# Winsock is required by every target that links mnet on Windows.
if(WIN32)
    target_link_libraries(mnet_static PUBLIC ws2_32)
    target_link_libraries(mnet_shared PRIVATE ws2_32)
endif()

# Examples
add_executable(example_http_server example/example_http_server.c)
target_link_libraries(example_http_server PRIVATE mnet_static)
target_include_directories(example_http_server PRIVATE example)

add_executable(example_api_server example/example_api_server.c)
target_link_libraries(example_api_server PRIVATE mnet_static)
target_include_directories(example_api_server PRIVATE example)

add_executable(example_combined example/example_combined.c)
target_link_libraries(example_combined PRIVATE mnet_static)
target_include_directories(example_combined PRIVATE example)

# Tests
enable_testing()
add_executable(test_mnet test/test_mnet.c)
target_link_libraries(test_mnet PRIVATE mnet_static)
target_include_directories(test_mnet PRIVATE include src test)
add_test(NAME mnet_tests COMMAND test_mnet)

add_executable(test_http test/test_http.c)
target_link_libraries(test_http PRIVATE mnet_static)
target_include_directories(test_http PRIVATE include src test)
add_test(NAME mnet_http_tests COMMAND test_http)

add_executable(test_parser test/test_mnet_parser.c)
target_link_libraries(test_parser PRIVATE mnet_static)
target_include_directories(test_parser PRIVATE include src test)
add_test(NAME mnet_parser_tests COMMAND test_parser)

add_executable(test_stress test/test_stress.c)
target_link_libraries(test_stress PRIVATE mnet_static)
target_include_directories(test_stress PRIVATE include src test)
add_test(NAME mnet_stress_tests COMMAND test_stress)

add_executable(test_features test/test_features.c)
target_link_libraries(test_features PRIVATE mnet_static)
target_include_directories(test_features PRIVATE include src test)
add_test(NAME mnet_feature_tests COMMAND test_features)

add_executable(test_security test/test_security.c)
target_link_libraries(test_security PRIVATE mnet_static)
target_include_directories(test_security PRIVATE include src test)
add_test(NAME mnet_security_tests COMMAND test_security)

add_executable(test_client test/test_client.c)
target_link_libraries(test_client PRIVATE mnet_static)
target_include_directories(test_client PRIVATE include src test)
add_test(NAME mnet_client_tests COMMAND test_client)

add_executable(test_welcome test/test_welcome.c)
target_link_libraries(test_welcome PRIVATE mnet_static)
target_include_directories(test_welcome PRIVATE include src test)
add_test(NAME mnet_welcome_tests COMMAND test_welcome)

add_executable(test_starvation test/test_starvation.c)
target_link_libraries(test_starvation PRIVATE mnet_static)
target_include_directories(test_starvation PRIVATE include src test)
add_test(NAME mnet_starvation_tests COMMAND test_starvation)

add_executable(test_reentrancy test/test_reentrancy.c)
target_link_libraries(test_reentrancy PRIVATE mnet_static)
target_include_directories(test_reentrancy PRIVATE include src test)
add_test(NAME mnet_reentrancy_tests COMMAND test_reentrancy)

add_executable(test_http_edge test/test_http_edge.c)
target_link_libraries(test_http_edge PRIVATE mnet_static)
target_include_directories(test_http_edge PRIVATE include src test)
add_test(NAME mnet_http_edge_tests COMMAND test_http_edge)

# Install
install(TARGETS mnet_static mnet_shared
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(DIRECTORY include/ DESTINATION include)

# pkg-config
configure_file(mnet.pc.in mnet.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/mnet.pc DESTINATION lib/pkgconfig)
