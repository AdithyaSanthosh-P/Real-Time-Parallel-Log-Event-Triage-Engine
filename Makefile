# Real-Time Parallel Log / Event Triage Engine — build
#
#   make                 bin/triage (+ tools that exist)
#   make test            unit tests (tests/test_*.c)
#   make difftest        DFA engine vs POSIX regexec on tests/corpus
#   make e2e             triage on tests/corpus/synthetic_small.log vs tests/expected/
#   make SAN=address     ASan/UBSan build;  make SAN=thread  for TSan
#   make clean
#
# Sanitizer builds use separate object/binary directories (build/<SAN>/,
# bin/<SAN>/) so they never mix with, or invalidate, the normal build.

ifeq ($(origin CC),default)
  CC := gcc
endif
SAN     ?=

CSTD    := -std=c11 -D_GNU_SOURCE
WARN    := -Wall -Wextra -Wpedantic
ifeq ($(SAN),)
  OPT      := -O2
  VARIANT  := plain
  BIN_DIR  := bin
else ifeq ($(SAN),address)
  OPT      := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
  VARIANT  := address
  BIN_DIR  := bin/address
else ifeq ($(SAN),thread)
  OPT      := -O1 -g -fsanitize=thread
  VARIANT  := thread
  BIN_DIR  := bin/thread
else
  $(error SAN must be empty, 'address' or 'thread' (got '$(SAN)'))
endif

CFLAGS  += $(CSTD) $(OPT) $(WARN) -pthread -MMD -MP
LDFLAGS += $(OPT) -pthread
LDLIBS  +=

BUILD   := build/$(VARIANT)

SRCS      := $(wildcard src/*.c)
CORE_SRCS := $(filter-out src/main.c,$(SRCS))
CORE_OBJS := $(CORE_SRCS:src/%.c=$(BUILD)/%.o)
MAIN_OBJ  := $(BUILD)/main.o

TEST_SRCS := $(wildcard tests/test_*.c)
TEST_BINS := $(TEST_SRCS:tests/%.c=$(BIN_DIR)/tests/%)

# Companion tools exist only once their tasks are done (gen: T6.3, difftest: T2.5).
TOOL_SRCS := $(wildcard tools/gen.c tools/difftest.c)
TOOL_BINS := $(TOOL_SRCS:tools/%.c=$(BIN_DIR)/%)

.PHONY: all test difftest e2e clean
.SECONDARY:

all: $(BIN_DIR)/triage $(TOOL_BINS)

$(BIN_DIR)/triage: $(MAIN_OBJ) $(CORE_OBJS)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

# Tools and tests link against the core objects (everything but main.o).
$(BUILD)/tools/%.o: tools/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Isrc -c -o $@ $<

$(BIN_DIR)/%: $(BUILD)/tools/%.o $(CORE_OBJS)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/tests/%.o: tests/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Isrc -c -o $@ $<

$(BIN_DIR)/tests/%: $(BUILD)/tests/%.o $(CORE_OBJS)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; $$t; done; \
	echo "all $(words $(TEST_BINS)) test programs passed"

# DFA vs per-rule regexec on every corpus line plus generated strings (§3.5).
DIFFTEST_RANDOM ?= 100000
difftest: all
	@if [ -x $(BIN_DIR)/difftest ]; then \
	    $(BIN_DIR)/difftest --random $(DIFFTEST_RANDOM) tests/corpus/*.log; \
	else \
	    echo "difftest: tools/difftest.c is missing" >&2; exit 1; \
	fi

e2e: all
	@if [ -x tools/run_e2e.sh ]; then \
	    tools/run_e2e.sh $(BIN_DIR)/triage; \
	else \
	    echo "e2e: not implemented yet (task T1.7)" >&2; exit 1; \
	fi

clean:
	rm -rf bin build

-include $(CORE_OBJS:.o=.d) $(MAIN_OBJ:.o=.d) $(TEST_SRCS:tests/%.c=$(BUILD)/tests/%.d) $(TOOL_SRCS:tools/%.c=$(BUILD)/tools/%.d)
