# Real-Time Parallel Log / Event Triage Engine — build
#
#   make                 bin/triage (+ tools that exist)
#   make test            unit tests (tests/test_*.c)
#   make difftest        DFA engine vs POSIX regexec on tests/corpus
#   make e2e             triage on tests/corpus/synthetic_small.log vs tests/expected/
#   make bp / bp-test    Phase R: tools/basepaper/ and its tests (bp-test also runs under make test)
#   make alloc-test      T4.2: no allocation growth with input size (also run by make test)
#   make gen-test        T6.3: generator reproducible, exact rates, truth equals engine labels (also run by make test)
#   make compare         T4.6: parallel runs == single-threaded reference on tests/corpus
#   make dashboard       Triage Desk: live dashboard at http://127.0.0.1:8765/ (tools/dashboard/)
#   make demo            review demo (tools/demo.sh, talking points in docs/demo_guide.md)
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
LDLIBS  += -lm

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

.PHONY: all test difftest e2e bp bp-test alloc-test gen-test compare demo dashboard print-flags clean
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
	@$(MAKE) --no-print-directory bp-test
	@$(MAKE) --no-print-directory alloc-test
	@$(MAKE) --no-print-directory gen-test
	@python3 -I tests/test_label_eval.py

# LD_PRELOAD allocation counter (plain builds only: the sanitizers own malloc).
ALLOC_SHIM := $(BIN_DIR)/tests/alloc_count.so
$(ALLOC_SHIM): tests/alloc_count.c
	@mkdir -p $(@D)
	$(CC) $(CSTD) -O2 $(WARN) -fPIC -shared -o $@ $< -ldl

alloc-test: $(BIN_DIR)/triage $(if $(SAN),,$(ALLOC_SHIM))
	@echo "== tests/test_alloc.sh"; tests/test_alloc.sh $(BIN_DIR) $(SAN)

dashboard: all
	@python3 tools/dashboard/server.py

gen-test: all
	@tests/test_gen.sh $(BIN_DIR)

compare: all
	@tools/compare_runs.sh $(BIN_DIR)/triage

demo: all
	@tools/demo.sh

# ---- Phase R: base-paper reproduction (spec §10.4). Independent of src/ ----
# Programs are tools/basepaper/bp_*.c except bp_lib.c, which they all link.
BP_LIB_OBJ   := $(BUILD)/basepaper/bp_lib.o
BP_PROG_SRCS := $(filter-out tools/basepaper/bp_lib.c,$(wildcard tools/basepaper/bp_*.c))
BP_BINS      := $(BP_PROG_SRCS:tools/basepaper/%.c=$(BIN_DIR)/%)
BP_TEST_BIN  := $(BIN_DIR)/tests/test_bp

$(BUILD)/basepaper/%.o: tools/basepaper/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Itools/basepaper -Isrc -c -o $@ $<

# bp_bench's `dfa` variant uses our compiler (Phase 2), so it links the core objects.
$(BIN_DIR)/bp_bench: $(CORE_OBJS)

$(BP_BINS): $(BIN_DIR)/%: $(BUILD)/basepaper/%.o $(BP_LIB_OBJ)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/basepaper/test_bp.o: tests/basepaper/test_bp.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Itools/basepaper -c -o $@ $<

$(BP_TEST_BIN): $(BUILD)/basepaper/test_bp.o $(BP_LIB_OBJ)
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

bp: $(BP_BINS)

# For results/basepaper/machine.txt: the exact compiler and flags used.
print-flags:
	@echo "$(CC) $(CFLAGS) $(LDFLAGS)"

bp-test: $(BP_BINS) $(BP_TEST_BIN)
	@echo "== $(BP_TEST_BIN)"; $(BP_TEST_BIN) $(BIN_DIR)/bp_fork
	@echo "== tests/basepaper/test_gen.sh"; tests/basepaper/test_gen.sh
	@echo "== tests/basepaper/test_variants.sh"; tests/basepaper/test_variants.sh $(BIN_DIR)
	@echo "== tests/basepaper/test_smoke.sh"; tests/basepaper/test_smoke.sh $(BIN_DIR)

# DFA vs per-rule regexec on every corpus line plus generated strings (§3.5).
DIFFTEST_RANDOM ?= 100000
difftest: all
	@if [ -x $(BIN_DIR)/difftest ]; then \
	    $(BIN_DIR)/difftest --random $(DIFFTEST_RANDOM) tests/corpus/*.log; \
	    for r in rules/bench_*.rules; do $(BIN_DIR)/difftest -r $$r --random $(DIFFTEST_RANDOM) tests/corpus/*.log || exit 1; done; \
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

-include $(wildcard $(BUILD)/basepaper/*.d)
-include $(CORE_OBJS:.o=.d) $(MAIN_OBJ:.o=.d) $(TEST_SRCS:tests/%.c=$(BUILD)/tests/%.d) $(TOOL_SRCS:tools/%.c=$(BUILD)/tools/%.d)
