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
# libffi, for `.extern` (the declarative FFI), if it can be found. FFI=0 builds without it.
ifneq ($(FFI),0)
FFI_CFLAGS := $(shell pkg-config --cflags libffi 2>/dev/null)
FFI_LIBS := $(shell pkg-config --libs libffi 2>/dev/null)
ifeq ($(FFI_LIBS),)
SDK_FFI := $(shell xcrun --show-sdk-path 2>/dev/null)/usr/include/ffi
ifneq ($(wildcard $(SDK_FFI)/ffi.h),)
FFI_CFLAGS := -I$(SDK_FFI)
FFI_LIBS := -lffi
endif
endif
ifneq ($(FFI_LIBS),)
FFI_CFLAGS += -DMLWS_HAVE_FFI
endif
endif
ifeq ($(shell uname -s),Linux)
DL_LIBS := -ldl -lpthread
endif
OBJ := $(SRC:%.c=$(BUILD)/%.o) $(BUILD)/gen/dashboard.o

.PHONY: all test test-ext check-gc check-tools check-asan check-tsan bench docs clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) -lm $(FFI_LIBS) $(DL_LIBS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(WARN) $(CFLAGS) $(FFI_CFLAGS) -MMD -MP -c $< -o $@

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

# Native extensions: the demo (extensions/demo) is the example and the test fixture.
EXT_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2 -g -fPIC -shared -Iinclude
build/ext/demo.mlwsx: extensions/demo/demo.c include/mlwsvm/mlwsvm.h
	@mkdir -p $(dir $@)
	$(CC) $(EXT_CFLAGS) -o $@ $<

# Trusted extension directories for tests/native: a good one, and three that
# must be refused (not in TRUSTED, changed since installed, writable by others).
TEST_EXT := build/test-ext
test-ext: $(BIN) build/ext/demo.mlwsx
	@rm -rf build/test-ext build/test-ext-unlisted build/test-ext-tampered build/test-ext-open
	@./$(BIN) ext install --dir $(TEST_EXT) build/ext/demo.mlwsx >/dev/null
	@./$(BIN) ext allow-ffi --dir $(TEST_EXT) - libm libm.so.6 >/dev/null
	@./$(BIN) ext allow-ffi --dir build/test-ext-unlisted libm >/dev/null
	@cp build/ext/demo.mlwsx build/test-ext-unlisted/
	@./$(BIN) ext install --dir build/test-ext-tampered build/ext/demo.mlwsx >/dev/null
	@printf 'x' >> build/test-ext-tampered/demo.mlwsx
	@./$(BIN) ext install --dir build/test-ext-open build/ext/demo.mlwsx >/dev/null
	@chmod o+w build/test-ext-open

test: $(BIN) test-ext
	./$(BIN) test tests examples

check-tools: $(BIN)
	sh tests/tools/check_tools.sh ./$(BIN)

check-gc: $(BIN) test-ext
	MLWSVM_GC_STRESS=1 ./$(BIN) test tests examples

check-asan: test-ext
	$(MAKE) BUILD=build-asan BIN=mlwsvm-asan \
	    CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" \
	    LDFLAGS="-fsanitize=address,undefined"
	$(ASAN_ENV) ./mlwsvm-asan test tests examples
	$(ASAN_ENV) MLWSVM_GC_STRESS=1 ./mlwsvm-asan test tests examples

check-tsan: test-ext
	$(MAKE) BUILD=build-tsan BIN=mlwsvm-tsan \
	    CFLAGS="-O1 -g -fsanitize=thread -fno-omit-frame-pointer" LDFLAGS="-fsanitize=thread"
	TSAN_OPTIONS=halt_on_error=1 MLWSVM_THREADS=4 ./mlwsvm-tsan test tests examples
	TSAN_OPTIONS=halt_on_error=1 MLWSVM_THREADS=8 ./mlwsvm-tsan test tests/processes tests/threads tests/native tests/os tests/net

bench: $(BIN)
	sh bench/run.sh ./$(BIN)

docs: $(BIN)
	./$(BIN) doc isa -o docs/ISA.md
	./$(BIN) doc natives -o docs/NATIVES.md

clean:
	rm -rf build build-asan build-tsan build-noffi mlwsvm mlwsvm-asan mlwsvm-tsan mlwsvm-noffi
