cmake_minimum_required(VERSION 3.16)
project(mnet VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_definitions(_POSIX_C_SOURCE=200112L)

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
target_include_directories(mnet_static PUBLIC include)
set_target_properties(mnet_static PROPERTIES OUTPUT_NAME mnet)

# Shared library
add_library(mnet_shared SHARED ${MNET_SOURCES})
target_include_directories(mnet_shared PUBLIC include)
set_target_properties(mnet_shared PROPERTIES
    OUTPUT_NAME mnet
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)

# Examples
add_executable(example_http_server example/example_http_server.c)
target_link_libraries(example_http_server PRIVATE mnet_static)

add_executable(example_api_server example/example_api_server.c)
target_link_libraries(example_api_server PRIVATE mnet_static)

add_executable(example_combined example/example_combined.c)
target_link_libraries(example_combined PRIVATE mnet_static)

# Tests
enable_testing()
add_executable(test_mnet test/test_mnet.c)
target_link_libraries(test_mnet PRIVATE mnet_static)
target_include_directories(test_mnet PRIVATE include src test)
add_test(NAME mnet_tests COMMAND test_mnet)

# Install
install(TARGETS mnet_static mnet_shared
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(DIRECTORY include/ DESTINATION include)

# pkg-config
configure_file(mnet.pc.in mnet.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/mnet.pc DESTINATION lib/pkgconfig)
