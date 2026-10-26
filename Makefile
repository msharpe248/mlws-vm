# MLWSVM build. Plain C11, no dependencies beyond libc and libm.
#
#   make              build ./mlwsvm
#   make test         run the assembly test suite and the examples
#   make check-tools  smoke-test --monitor, top, --profile and --trace
#   make check-gc     run the tests with a garbage collection at every allocation
#   make check-asan   build with AddressSanitizer + UBSan and run the tests (normal and GC stress)
#   make check-tsan   build with ThreadSanitizer and run the tests on several worker threads
#   make bench        run the benchmarks in bench/
#   make docs         regenerate docs/ISA.md and docs/NATIVES.md from the code

CC      ?= cc
CFLAGS  ?= -O2 -g
WARN    := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter
BUILD   ?= build
BIN     ?= mlwsvm

SRC := $(wildcard src/*.c src/*/*.c)

# LeakSanitizer is only available on Linux.
ifeq ($(shell uname -s),Linux)
ASAN_ENV := ASAN_OPTIONS=detect_leaks=1
else
ASAN_ENV :=
endif
OBJ := $(SRC:%.c=$(BUILD)/%.o) $(BUILD)/gen/dashboard.o

.PHONY: all test check-gc check-tools check-asan check-tsan bench docs clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) -lm

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(WARN) $(CFLAGS) -MMD -MP -c $< -o $@

# The web dashboard is compiled into the binary (served by --monitor).
$(BUILD)/embed: build-tools/embed.c
	@mkdir -p $(dir $@)
	$(CC) -O1 -o $@ $<

$(BUILD)/gen/dashboard.c: web/dashboard.html $(BUILD)/embed
	@mkdir -p $(dir $@)
	$(BUILD)/embed web/dashboard.html DASHBOARD_HTML > $@

$(BUILD)/gen/dashboard.o: $(BUILD)/gen/dashboard.c
	$(CC) $(CFLAGS) -c $< -o $@

-include $(OBJ:.o=.d)

test: $(BIN)
	./$(BIN) test tests examples

check-tools: $(BIN)
	sh tests/tools/check_tools.sh ./$(BIN)

check-gc: $(BIN)
	MLWSVM_GC_STRESS=1 ./$(BIN) test tests examples

check-asan:
	$(MAKE) BUILD=build-asan BIN=mlwsvm-asan \
	    CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" \
	    LDFLAGS="-fsanitize=address,undefined"
	$(ASAN_ENV) ./mlwsvm-asan test tests examples
	$(ASAN_ENV) MLWSVM_GC_STRESS=1 ./mlwsvm-asan test tests examples

check-tsan:
	$(MAKE) BUILD=build-tsan BIN=mlwsvm-tsan \
	    CFLAGS="-O1 -g -fsanitize=thread -fno-omit-frame-pointer" LDFLAGS="-fsanitize=thread"
	TSAN_OPTIONS=halt_on_error=1 MLWSVM_THREADS=4 ./mlwsvm-tsan test tests examples
	TSAN_OPTIONS=halt_on_error=1 MLWSVM_THREADS=8 ./mlwsvm-tsan test tests/processes tests/threads

bench: $(BIN)
	sh bench/run.sh ./$(BIN)

docs: $(BIN)
	./$(BIN) doc isa -o docs/ISA.md
	./$(BIN) doc natives -o docs/NATIVES.md

clean:
	rm -rf build build-asan build-tsan mlwsvm mlwsvm-asan mlwsvm-tsan
