CC      = gcc
CFLAGS  = -std=c17 -Wall -Wextra -Wpedantic -Werror \
          -D_POSIX_C_SOURCE=200112L -Iinclude

# Source files
SRCS    = src/mnet.c src/mnet_app.c src/mnet_response.c \
          src/mnet_request.c src/mnet_router.c

.PHONY: examples test clean help

# Build all examples
examples: example/example_http_server \
          example/example_api_server \
          example/example_combined

example/example_http_server: $(SRCS) example/example_http_server.c
	$(CC) $(CFLAGS) $(SRCS) example/example_http_server.c -o $@

example/example_api_server: $(SRCS) example/example_api_server.c
	$(CC) $(CFLAGS) $(SRCS) example/example_api_server.c -o $@

example/example_combined: $(SRCS) example/example_combined.c
	$(CC) $(CFLAGS) $(SRCS) example/example_combined.c -o $@

# Compile a .c file with mnet (e.g. make main builds from main.c)
%:: %.c $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) $< -o $@

# Run tests (builds + runs)
test: clean test_mnet
	./test_mnet

test_mnet: $(SRCS) test/test_mnet.c
	$(CC) $(CFLAGS) -Itest $(SRCS) test/test_mnet.c -o $@

# Clean
clean:
	rm -f mnet-server test_mnet
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
