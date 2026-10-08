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

# Source files
SRCS    = src/mnet.c src/mnet_app.c src/mnet_response.c \
          src/mnet_request.c src/mnet_router.c

.PHONY: examples test clean help

# Build all examples
examples: example/example_http_server \
          example/example_api_server \
          example/example_combined

example/example_http_server: $(SRCS) example/example_http_server.c
	$(CC) $(CFLAGS) -Iexample $(SRCS) example/example_http_server.c -o $@ $(LDFLAGS)

example/example_api_server: $(SRCS) example/example_api_server.c
	$(CC) $(CFLAGS) -Iexample $(SRCS) example/example_api_server.c -o $@ $(LDFLAGS)

example/example_combined: $(SRCS) example/example_combined.c
	$(CC) $(CFLAGS) -Iexample $(SRCS) example/example_combined.c -o $@ $(LDFLAGS)

# Compile a .c file with mnet (e.g. make main builds from main.c)
%:: %.c $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) $< -o $@

# Run tests (builds + runs)
test: clean test_mnet test_http test_parser test_stress test_features test_security
	./test_mnet
	./test_http
	./test_parser
	./test_stress
	./test_features
	./test_security

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

# Run tests (builds + runs)
test: clean test_mnet test_http test_parser test_stress test_features test_security test_client
clean:
	rm -f mnet-server test_mnet test_http test_parser test_stress test_features fuzz_http
	rm -f example/example_http_server
	rm -f example/example_api_server
	rm -f example/example_combined
	rm -f *.o

# Help
help:
	@echo "mnet - available targets:"
	@echo "  make examples  Build all example binaries"
	@echo "  make test      Run the test suite (builds + runs)"
	@echo "  make clean     Remove all built binaries"
	@echo "  make help      Show this help"
	@echo ""
	@echo "  make <name>    Build <name>.c linked with mnet"
	@echo "                  (e.g. make main builds ./main from main.c)"
