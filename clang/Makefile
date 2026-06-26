# ffz — standalone C fuzzy matcher. Requires a C11 compiler (gcc/clang).
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Iinclude
BUILD   := build
SRCS    := $(wildcard src/*.c)

# Threads: Win32 API needs no extra lib; POSIX needs -pthread.
ifneq ($(OS),Windows_NT)
CFLAGS  += -pthread
endif

.PHONY: all test lib size clean
all: test

# Build and run the test suite.
test:
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SRCS) tests/test_ffz.c -o $(BUILD)/test_ffz
	$(BUILD)/test_ffz

# Memory-leak smoke test (counting allocator).
leak:
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DFFZ_TRACK_ALLOC $(SRCS) tests/test_leak.c -o $(BUILD)/test_leak
	$(BUILD)/test_leak

# Static library.
lib:
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $(SRCS)
	ar rcs $(BUILD)/libffz.a *.o
	@rm -f *.o

# Size-optimized, matcher-only section sizes (per the README size discussion).
# GNU `size` folds read-only data into the "text" column.
size:
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -Os -ffunction-sections -fdata-sections -Iinclude -c $(SRCS)
	@echo "--- per object (-Os) ---"
	@size *.o
	@echo "--- matcher total (text = code + rodata) ---"
	@size -t *.o | tail -1
	@rm -f *.o

clean:
	rm -rf $(BUILD) *.o
