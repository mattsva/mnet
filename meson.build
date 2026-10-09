project('mnet', 'c',
    version: '0.2.5',
    default_options: ['c_std=c17'],
)

# The same warning levels as the Makefile: everything on, warnings are errors.
cc = meson.get_compiler('c')
warning_flags = ['-Wall', '-Wextra', '-Wpedantic', '-Werror']
if cc.get_id() == 'msvc'
    add_project_arguments(['/W4', '/WX'], language: 'c')
    # Suppress false-positive C4996 deprecation warnings for standard C functions.
    add_project_arguments(['/D_CRT_SECURE_NO_WARNINGS', '/D_CRT_NONSTDC_NO_DEPRECATE'], language: 'c')
    # MSVC's buffer-security hardening is on by default: /GS (stack cookies),
    # /DYNAMICBASE (ASLR) and /NXCOMPAT (DEP) are all enabled unless explicitly
    # turned off, so no extra flags are needed for parity with the POSIX side.
else
    add_project_arguments(warning_flags, language: 'c')

    # Hardening. -fstack-protector-strong works at any optimization level.
    add_project_arguments('-fstack-protector-strong', language: 'c')

    # _FORTIFY_SOURCE=2 only makes sense with optimization: without it glibc
    # emits a #warning that -Werror would escalate. Meson's debug buildtype
    # is -O0, so restrict it to optimized builds.
    if get_option('optimization') != '0'
        add_project_arguments('-D_FORTIFY_SOURCE=2', language: 'c')
    endif

    # relro/now are ELF-only.
    if host_machine.system() == 'linux'
        add_project_link_arguments('-Wl,-z,relro,-z,now', language: 'c')
    endif
endif

# glibc's _FORTIFY_SOURCE is only meaningful with optimization, and some
# toolchains (notably the Nix gcc wrapper) define it unconditionally. At -O0,
# which is Meson's default for the debug buildtype, glibc then emits
# "#warning _FORTIFY_SOURCE requires compiling with optimization (-O)" and
# -Werror would fail the build over it. The macro cannot be undefined from the
# command line, so keep preprocessor notices non-fatal in unoptimized builds.
# Ordinary diagnostics (-Wall/-Wextra/-Wpedantic) remain errors.
if cc.get_id() != 'msvc' and get_option('optimization') == '0'
    add_project_arguments('-Wno-error=cpp', language: 'c')
endif

add_project_arguments('-D_POSIX_C_SOURCE=200112L', language: 'c')

if host_machine.system() == 'darwin'
    add_project_arguments('-D_DARWIN_C_SOURCE', language: 'c')
endif

# The worker pool needs threads on POSIX.
thread_dep = dependency('threads')

# Optional sanitizer build: meson setup build -Dsanitize=true
if get_option('sanitize')
    add_project_arguments('-fsanitize=address,undefined',
        '-fno-omit-frame-pointer', '-g', language: 'c')
    add_project_link_arguments('-fsanitize=address,undefined', language: 'c')
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
mnet_priv_inc = include_directories('include/mnet')

# Static library
mnet_static = static_library('mnet',
    mnet_sources,
    include_directories: [mnet_inc, mnet_priv_inc],
    dependencies: thread_dep,
)

# Shared library
mnet_shared = shared_library('mnet',
    mnet_sources,
    include_directories: [mnet_inc, mnet_priv_inc],
    dependencies: thread_dep,
    version: meson.project_version(),
    soversion: '1',
)

example_inc = include_directories('example')

# Examples
example_http_server = executable('example_http_server',
    'example/example_http_server.c',
    link_with: mnet_static,
    include_directories: [mnet_inc, example_inc],
)

example_api_server = executable('example_api_server',
    'example/example_api_server.c',
    link_with: mnet_static,
    include_directories: [mnet_inc, example_inc],
)

example_combined = executable('example_combined',
    'example/example_combined.c',
    link_with: mnet_static,
    include_directories: [mnet_inc, example_inc],
)

# Tests
test_mnet = executable('test_mnet',
    'test/test_mnet.c',
    link_with: mnet_static,
    include_directories: [mnet_inc, include_directories('src'), include_directories('test')],
)

test('mnet_tests', test_mnet)

test_http = executable('test_http',
    'test/test_http.c',
    link_with: mnet_static,
    include_directories: [mnet_inc, include_directories('src'), include_directories('test')],
)

test('mnet_http_tests', test_http)

test_parser = executable('test_parser',
    'test/test_mnet_parser.c',
    link_with: mnet_static,
    include_directories: [mnet_inc, include_directories('src'), include_directories('test')],
)

test('mnet_parser_tests', test_parser)

test_stress = executable('test_stress',
    'test/test_stress.c',
    link_with: mnet_static,
    include_directories: [mnet_inc, include_directories('src'), include_directories('test')],
)

test('mnet_stress_tests', test_stress)

test_features = executable('test_features',
    'test/test_features.c',
    link_with: mnet_static,
    include_directories: [mnet_inc, include_directories('src'), include_directories('test')],
)

test('mnet_feature_tests', test_features)

test_security = executable('test_security',
    'test/test_security.c',
    link_with: mnet_static,
    include_directories: [mnet_inc, include_directories('src'), include_directories('test')],
)

test('mnet_security_tests', test_security)

test_starvation = executable('test_starvation',
    'test/test_starvation.c',
    link_with: mnet_static,
    include_directories: [mnet_inc, include_directories('src'), include_directories('test')],
)

test('mnet_starvation_tests', test_starvation)

test_reentrancy = executable('test_reentrancy',
    'test/test_reentrancy.c',
    link_with: mnet_static,
    include_directories: [mnet_inc, include_directories('src'), include_directories('test')],
)

test('mnet_reentrancy_tests', test_reentrancy)

test_http_edge = executable('test_http_edge',
    'test/test_http_edge.c',
    link_with: mnet_static,
    include_directories: [mnet_inc, include_directories('src'), include_directories('test')],
)

test('mnet_http_edge_tests', test_http_edge)

test_client = executable('test_client',
    'test/test_client.c',
    link_with: mnet_static,
    include_directories: [mnet_inc, include_directories('src'), include_directories('test')],
)

test('mnet_client_tests', test_client)

test_welcome = executable('test_welcome',
    'test/test_welcome.c',
    link_with: mnet_static,
    include_directories: [mnet_inc, include_directories('src'), include_directories('test')],
)

test('mnet_welcome_tests', test_welcome)

# Install headers (libraries are installed by default)
install_headers('include/mnet/mnet.h', 'include/mnet/mnet_app.h', 'include/mnet/mnet_request.h',
    'include/mnet/mnet_response.h', 'include/mnet/mnet_router.h', 'include/mnet/mnet_socket.h',
    'include/mnet/mnet_compat.h',
    subdir: 'mnet')

# pkg-config
import('pkgconfig').generate(mnet_static,
    name: 'mnet',
    description: 'A simple, ergonomic web framework for C',
    version: meson.project_version()
)
