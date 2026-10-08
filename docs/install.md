# mnet - Installation Guide

## Requirements

- A C17 compiler (GCC, Clang, or MSVC)
- Linux, macOS, BSD, or Windows
- Threads: pthreads on POSIX, the Win32 thread API on Windows
- No other external dependencies

## Building from source

### Using Make (default)

```sh
make              # Build examples and test_mnet
make test         # Build and run tests
make clean        # Remove build artifacts
```

### Using CMake

```sh
mkdir build && cd build
cmake ..
make
ctest             # Run tests
```

### Using Meson

```sh
meson setup build
meson compile -C build
meson test -C build
```

## Installing

### From source (make)

```sh
make
sudo make install
```

### From source (CMake)

```sh
mkdir build && cd build
cmake ..
make
sudo make install
```

### From source (Meson)

```sh
meson setup build
meson compile -C build
sudo meson install -C build
```

## Installing via package manager

mnet is **not** available in any package manager. Build from source instead.

### From source (make)

```sh
make
sudo make install
```

### Linking against mnet

After building, you can link against the static or shared library:

```sh
# Static library
gcc -Iinclude myapp.c build/libmnet.a -o myapp

# Shared library
gcc -Iinclude myapp.c -Lbuild -lmnet -o myapp
```

Or use pkg-config (if installed):

```sh
gcc $(pkg-config --cflags --libs mnet) myapp.c -o myapp
```

## Linking against mnet

After building, you can link against the static or shared library:

```sh
# Static library
gcc -Iinclude myapp.c build/libmnet.a -o myapp

# Shared library
gcc -Iinclude myapp.c -Lbuild -lmnet -o myapp
```

Or use pkg-config (if installed):

```sh
gcc $(pkg-config --cflags --libs mnet) myapp.c -o myapp
```

## Requirements for development

- Git
- C17 compiler
- Make or CMake or Meson
- Valgrind (optional, for memory leak detection)
- AddressSanitizer and UndefinedBehaviorSanitizer (optional, for memory safety checks)
- ThreadSanitizer (optional, for concurrency bug detection)
- libFuzzer or AFL++ (optional, for fuzz testing)
