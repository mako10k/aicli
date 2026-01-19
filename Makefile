CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS ?=
LDFLAGS ?=

BIN_DIR := bin
SRC_DIR := src
OBJ_DIR := build

CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS := $(shell pkg-config --libs libcurl 2>/dev/null)

# yyjson is header-only + optional .c; this scaffold assumes header is available.
# If you vendor yyjson, add it to SRC.

SRCS := \
	$(SRC_DIR)/main.c \
	$(SRC_DIR)/cli.c \
	$(SRC_DIR)/config.c \
	$(SRC_DIR)/brave_search.c \
	$(SRC_DIR)/execute_dsl.c \
	$(SRC_DIR)/execute_tool_impl.c \
	$(SRC_DIR)/path_util.c \
	$(SRC_DIR)/paging_cache.c

OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

.PHONY: all clean test

all: $(BIN_DIR)/aicli

$(BIN_DIR)/aicli: $(OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(CURL_LIBS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(CURL_CFLAGS) -Iinclude -c -o $@ $<

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

test: $(BIN_DIR)/aicli
	./tests/run_tests.sh
