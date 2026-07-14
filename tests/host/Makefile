# Host-side unit tests for code that's worth testing standalone, on the
# dev machine, without the Move device or its toolchain.
#
# Currently:
#   - test_midi_inject_writer — exercises the lock-free MPSC helper for
#     the /schwung-midi-inject ring with multi-threaded producers.
#
# Usage: `make -C tests/host test`

CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=c11
INCLUDES = -I../../src/host
LDFLAGS = -lpthread

ifeq ($(shell uname -s),Darwin)
GC_SECTIONS_LDFLAGS = -Wl,-dead_strip
else
GC_SECTIONS_LDFLAGS = -Wl,--gc-sections
endif

BUILD_DIR = ../../build/tests/host

TARGETS = $(BUILD_DIR)/test_midi_inject_writer $(BUILD_DIR)/test_display_clip_rect

.PHONY: all test clean

all: $(TARGETS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/test_display_clip_rect: test_display_clip_rect.c ../../src/host/js_display.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections \
		-I../../src -I../../src/lib -I../../src/host \
		-I../../libs/quickjs/quickjs-2025-04-26 \
		test_display_clip_rect.c ../../src/host/js_display.c \
		-o $@ $(GC_SECTIONS_LDFLAGS) -lm

$(BUILD_DIR)/%: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< -o $@ $(LDFLAGS)

test: $(TARGETS)
	@for t in $(TARGETS); do \
		echo "==> $$t"; \
		$$t || exit 1; \
	done

clean:
	rm -rf $(BUILD_DIR)
