# =============================================================================
#  mnet - Makefile
# =============================================================================
#  Targets:
#    make build FILE=prog SRC=main.c     Build a single C program linked with mnet
#    make examples                       Build all example programs
#    make test                           Build and run the full test suite
#    make clean                          Remove all build artifacts
#    make help                           Show this help message
#
#  Variables:
#    CC      - C compiler (default: gcc)
#    CFLAGS  - Compiler flags
#    LDFLAGS - Linker flags
#    SRCS    - Library source files for mnet
# =============================================================================

CC      = gcc
CFLAGS  = -std=c17 -Wall -Wextra -Wpedantic -Werror \
          -D_POSIX_C_SOURCE=200112L -Iinclude
LDFLAGS =

ifeq ($(shell uname -s),Darwin)
    CFLAGS += -D_DARWIN_C_SOURCE
endif

# The worker pool needs pthreads on POSIX.
ifeq ($(OS),Windows_NT)
    LDFLAGS += -lws2_32
else
    LDFLAGS += -pthread
    CFLAGS  += -pthread
endif

# Library source files for mnet
SRCS    = src/mnet.c src/mnet_app.c src/mnet_response.c \
          src/mnet_request.c src/mnet_router.c

# Default target: build everything
all: examples

# ---- Help ----

.PHONY: help
help:
	@echo "======================================================================"
	@echo "  mnet - Makefile targets"
	@echo "======================================================================"
	@echo ""
	@echo "  make build FILE=prog SRC=main.c     Build a single C program linked with mnet"
	@echo "  make build FILE=prog                Build a binary; SRC must be provided"
	@echo ""
	@echo "  make run EXAMPLE=example_http_server PORT=8080  Build + run an example"
	@echo ""
	@echo "  make examples                       Build all example programs"
	@echo ""
	@echo "  make test                           Build and run the full test suite"
	@echo ""
	@echo "  make clean                          Remove all build artifacts"
	@echo ""
	@echo "  make help                           Show this help message"
	@echo ""
	@echo "  make                              Default target (builds all examples)"
	@echo "======================================================================"

# ---- Examples ----

.PHONY: examples
examples: example/example_http_server \
          example/example_api_server \
          example/example_combined

example/example_http_server: $(SRCS) example/example_http_server.c
	$(CC) $(CFLAGS) -Iexample $(SRCS) example/example_http_server.c -o $@ $(LDFLAGS)

example/example_api_server: $(SRCS) example/example_api_server.c
	$(CC) $(CFLAGS) -Iexample $(SRCS) example/example_api_server.c -o $@ $(LDFLAGS)

example/example_combined: $(SRCS) example/example_combined.c
	$(CC) $(CFLAGS) -Iexample $(SRCS) example/example_combined.c -o $@ $(LDFLAGS)

# ---- Build a single program ----
# Usage: make build FILE=prog SRC=main.c
#   FILE=prog  Output binary name
#   SRC=...    Application source file
#   SRCS=...   Library source files (default: mnet library sources)
.PHONY: build
build:
	@if [ -z "$(FILE)" ]; then \
		echo "Usage: make build FILE=prog SRC=main.c [SRCS=...]"; \
		exit 1; \
	fi; \
	if [ -z "$(SRCS)" ]; then \
		SRCS='src/mnet.c src/mnet_app.c src/mnet_response.c src/mnet_request.c src/mnet_router.c'; \
	fi; \
	$(CC) $(CFLAGS) $(SRCS) $(SRC) -o $(FILE) $(LDFLAGS)

# ---- Tests ----

.PHONY: test
test: clean test_mnet test_http test_parser test_stress test_features test_security test_client test_welcome
	./test_welcome

test_mnet: $(SRCS) test/test_mnet.c
	$(CC) $(CFLAGS) -Itest $(SRCS) test/test_mnet.c -o $@ $(LDFLAGS)

test_http: $(SRCS) test/test_http.c
	$(CC) $(CFLAGS) -Isrc $(SRCS) test/test_http.c -o $@ $(LDFLAGS)

test_parser: $(SRCS) test/test_mnet_parser.c
	$(CC) $(CFLAGS) -Isrc $(SRCS) test/test_mnet_parser.c -o $@ $(LDFLAGS)

test_stress: $(SRCS) test/test_stress.c
	$(CC) $(CFLAGS) -Isrc $(SRCS) test/test_stress.c -o $@ $(LDFLAGS)

test_features: $(SRCS) test/test_features.c
	$(CC) $(CFLAGS) -Isrc $(SRCS) test/test_features.c -o $@ $(LDFLAGS)

test_security: $(SRCS) test/test_security.c
	$(CC) $(CFLAGS) -Isrc $(SRCS) test/test_security.c -o $@ $(LDFLAGS)

test_client: $(SRCS) test/test_client.c
	$(CC) $(CFLAGS) -Isrc $(SRCS) test/test_client.c -o $@ $(LDFLAGS)

test_welcome: $(SRCS) test/test_welcome.c
	$(CC) $(CFLAGS) -Isrc $(SRCS) test/test_welcome.c -o $@ $(LDFLAGS)

# ---- Run the examples (build + run) ----
# Usage: make run EXAMPLE=example_http_server PORT=8080
#   EXAMPLE  Example binary to build and run (default: example_http_server)
#   PORT     Port to listen on (default: 8080)
.PHONY: run
run:
	@echo "Starting mnet server..."
	@if [ -z "$(EXAMPLE)" ]; then EXAMPLE=example_http_server; fi
	@if [ -z "$(PORT)" ]; then PORT=8080; fi
	$(MAKE) examples
	./$(EXAMPLE) $(PORT)

# ---- Clean ----

.PHONY: clean
clean:
	rm -f mnet-server test_mnet test_http test_parser test_stress test_features test_security test_client test_welcome
	rm -f example/example_http_server
	rm -f example/example_api_server
	rm -f example/example_combined
	rm -f *.o
