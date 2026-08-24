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

BUILD_DIR = ../../build/tests/host

TARGETS = $(BUILD_DIR)/test_midi_inject_writer \
	$(BUILD_DIR)/test_overtake_native_led_repaint \
	$(BUILD_DIR)/test_shadow_midi_filter \
	$(BUILD_DIR)/test_midi_in_compact \
	$(BUILD_DIR)/test_rt_thread_audit \
	$(BUILD_DIR)/test_master_fx_slot_routing \
	$(BUILD_DIR)/test_master_fx_saved_state

# Read the Master FX cap out of the shipped header instead of restating it, so
# the routing test covers whatever range Master FX actually runs with. Raising
# MASTER_FX_SLOTS widens the test automatically; a test that hard-coded 4 would
# go on passing while covering half the slots.
MASTER_FX_SLOTS := $(shell awk '/^\#define MASTER_FX_SLOTS /{print $$3}' ../../src/host/shadow_chain_mgmt.h)
ifeq ($(strip $(MASTER_FX_SLOTS)),)
$(error could not read MASTER_FX_SLOTS from ../../src/host/shadow_chain_mgmt.h)
endif

.PHONY: all test clean

all: $(TARGETS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_overtake_native_led_repaint: test_overtake_native_led_repaint.c ../../src/host/shadow_led_queue.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L $(INCLUDES) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_shadow_midi_filter: test_shadow_midi_filter.c ../../src/host/shadow_midi_filter.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_midi_in_compact: test_midi_in_compact.c ../../src/host/shadow_midi_filter.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_rt_thread_audit: test_rt_thread_audit.c ../../src/host/rt_thread_audit.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_master_fx_slot_routing: test_master_fx_slot_routing.c ../../src/host/master_fx_key.h ../../src/host/shadow_chain_mgmt.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -DTEST_MASTER_FX_SLOTS=$(MASTER_FX_SLOTS) $< -o $@ $(LDFLAGS)

test: $(TARGETS)
	@for t in $(TARGETS); do \
		echo "==> $$t"; \
		$$t || exit 1; \
	done

clean:
	rm -rf $(BUILD_DIR)
