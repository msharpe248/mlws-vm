# MLWSVM build. Plain C11, no dependencies beyond libc and libm.
#
#   make              build ./mlwsvm
#   make test         run the assembly test suite and the examples
#   make check-gc     run the tests with a garbage collection at every allocation
#   make check-asan   build with AddressSanitizer + UBSan and run the tests (normal and GC stress)
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
OBJ := $(SRC:%.c=$(BUILD)/%.o)

.PHONY: all test check-gc check-asan docs clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) -lm

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(WARN) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(OBJ:.o=.d)

test: $(BIN)
	./$(BIN) test tests examples

check-gc: $(BIN)
	MLWSVM_GC_STRESS=1 ./$(BIN) test tests examples

check-asan:
	$(MAKE) BUILD=build-asan BIN=mlwsvm-asan \
	    CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" \
	    LDFLAGS="-fsanitize=address,undefined"
	$(ASAN_ENV) ./mlwsvm-asan test tests examples
	$(ASAN_ENV) MLWSVM_GC_STRESS=1 ./mlwsvm-asan test tests examples

docs: $(BIN)
	./$(BIN) doc isa -o docs/ISA.md
	./$(BIN) doc natives -o docs/NATIVES.md

clean:
	rm -rf build build-asan mlwsvm mlwsvm-asan
