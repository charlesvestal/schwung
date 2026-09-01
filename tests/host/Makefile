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

# Track HEADER dependencies. Several of the units under test are header-only
# (ext_midi_ring.h, fx_midi_filter.h, master_fx_key.h, chain_permute.h), and
# without this the rules below depend on the .c alone — so editing the header
# leaves a stale binary and `make test` reports PASS against the code you just
# changed. CI does a clean build and never saw it; a local mutation check does,
# and silently concluded the mutation was harmless. -MMD emits a .d per target,
# -MP adds phony targets so a DELETED header does not wedge the build.
# -MF names ONE dep file explicitly. Without it, -MMD on a rule with several
# sources asks the compiler to emit several .d files while -o names one output;
# gcc tolerates that, clang refuses ("cannot specify -o when generating multiple
# output files"). CI is gcc, so it went green; the local failure only appears
# once the multi-source targets actually rebuild, which is why it survived the
# commit that introduced it.
DEPFLAGS = -MMD -MP -MF $@.d

BUILD_DIR = ../../build/tests/host

TARGETS = $(BUILD_DIR)/test_midi_inject_writer \
	$(BUILD_DIR)/test_shadow_midi_out_capacity \
	$(BUILD_DIR)/test_overtake_midi_route \
	$(BUILD_DIR)/test_ext_midi_ring \
	$(BUILD_DIR)/test_ui_midi_out_carry \
	$(BUILD_DIR)/test_overtake_native_led_repaint \
	$(BUILD_DIR)/test_shadow_midi_filter \
	$(BUILD_DIR)/test_midi_in_compact \
	$(BUILD_DIR)/test_rt_thread_audit \
	$(BUILD_DIR)/test_master_fx_slot_routing \
	$(BUILD_DIR)/test_master_fx_saved_state \
	$(BUILD_DIR)/test_master_fx_snapshot \
	$(BUILD_DIR)/test_spi_tally \
	$(BUILD_DIR)/test_align_capture \
	$(BUILD_DIR)/test_host_api_reserved_tail \
	$(BUILD_DIR)/test_ui_flags_layout \
	$(BUILD_DIR)/test_recall_quantize \
	$(BUILD_DIR)/test_metronome_click

# Read the Master FX cap out of the shipped header instead of restating it, so
# the routing test covers whatever range Master FX actually runs with. Raising
# MASTER_FX_SLOTS widens the test automatically; a test that hard-coded 4 would
# go on passing while covering half the slots.
MASTER_FX_SLOTS := $(shell awk '/^\#define MASTER_FX_SLOTS /{print $$3}' ../../src/host/shadow_chain_mgmt.h)
ifeq ($(strip $(MASTER_FX_SLOTS)),)
$(error could not read MASTER_FX_SLOTS from ../../src/host/shadow_chain_mgmt.h)
endif

.DEFAULT_GOAL := all
.PHONY: all test clean

all: $(TARGETS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) $< -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_overtake_native_led_repaint: test_overtake_native_led_repaint.c ../../src/host/shadow_led_queue.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -D_POSIX_C_SOURCE=200809L $(INCLUDES) $(filter %.c,$^) -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_overtake_midi_route: test_overtake_midi_route.c ../../src/host/shadow_overtake_midi.c ../../src/host/shadow_midi_filter.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(filter %.c,$^) -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_shadow_midi_filter: test_shadow_midi_filter.c ../../src/host/shadow_midi_filter.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) $(filter %.c,$^) -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_midi_in_compact: test_midi_in_compact.c ../../src/host/shadow_midi_filter.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) $(filter %.c,$^) -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_rt_thread_audit: test_rt_thread_audit.c ../../src/host/rt_thread_audit.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) $(filter %.c,$^) -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_spi_tally: test_spi_tally.c ../../src/host/spi_tally.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) $(filter %.c,$^) -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_align_capture: test_align_capture.c ../../src/host/align_capture.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(filter %.c,$^) -o $@ $(LDFLAGS)

$(BUILD_DIR)/test_master_fx_snapshot: test_master_fx_snapshot.c ../../src/host/master_fx_snapshot.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< -o $@ $(LDFLAGS)

# -lm on top of the shared LDFLAGS: the click voice is a decaying sine, so
# metronome_click.h calls sinf/expf. The generic pattern rule above does not
# link libm, and on glibc the failure is a link error rather than anything
# subtle.
$(BUILD_DIR)/test_metronome_click: test_metronome_click.c ../../src/host/metronome_click.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) $< -o $@ $(LDFLAGS) -lm

$(BUILD_DIR)/test_master_fx_slot_routing: test_master_fx_slot_routing.c ../../src/host/master_fx_key.h ../../src/host/shadow_chain_mgmt.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) -DTEST_MASTER_FX_SLOTS=$(MASTER_FX_SLOTS) $< -o $@ $(LDFLAGS)

test: $(TARGETS)
	@for t in $(TARGETS); do \
		echo "==> $$t"; \
		$$t || exit 1; \
	done

clean:
	rm -rf $(BUILD_DIR)

# Header dependencies, included LAST. An included makefile's first target can
# become the default goal, which is why .DEFAULT_GOAL is set above — without it
# `make -C tests/host` built one target and said "up to date" for the rest.
-include $(TARGETS:=.d)
