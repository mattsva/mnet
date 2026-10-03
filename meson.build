project('mnet', 'c',
    version: '0.1.0',
    default_options: ['c_std=c17', 'c_args=-D_POSIX_C_SOURCE=200112L']
)

if host_machine.system() == 'darwin'
    add_project_arguments('-D_DARWIN_C_SOURCE', language: 'c')
endif

# Source files
mnet_sources = files(
    'src/mnet.c',
    'src/mnet_app.c',
    'src/mnet_response.c',
    'src/mnet_request.c',
    'src/mnet_router.c',
)

# Include directories
mnet_inc = include_directories('include')

# Static library
mnet_static = static_library('mnet',
    mnet_sources,
    include_directories: mnet_inc,
)

# Shared library
mnet_shared = shared_library('mnet',
    mnet_sources,
    include_directories: mnet_inc,
    version: meson.project_version(),
    soversion: '1',
)

# Examples
example_http_server = executable('example_http_server',
    'example/example_http_server.c',
    link_with: mnet_static,
    include_directories: mnet_inc,
)

example_api_server = executable('example_api_server',
    'example/example_api_server.c',
    link_with: mnet_static,
    include_directories: mnet_inc,
)

example_combined = executable('example_combined',
    'example/example_combined.c',
    link_with: mnet_static,
    include_directories: mnet_inc,
)

# Tests
test_mnet = executable('test_mnet',
    'test/test_mnet.c',
    link_with: mnet_static,
    include_directories: [mnet_inc, include_directories('src'), include_directories('test')],
)

test('mnet_tests', test_mnet)

# Install headers (libraries are installed by default)
install_headers('include/mnet.h', 'include/mnet_app.h', 'include/mnet_request.h',
    'include/mnet_response.h', 'include/mnet_router.h', 'include/mnet_socket.h',
    subdir: 'mnet')

# pkg-config
import('pkgconfig').generate(mnet_static,
    name: 'mnet',
    description: 'A simple, ergonomic web framework for C',
    version: '0.1.0'
)
