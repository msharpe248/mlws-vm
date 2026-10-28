# MLWSVM build. Plain C11; needs only libc and libm (plus -ldl and -lpthread on
# Linux). libffi is optional: when found, it enables the FFI (.extern).
#
#   make              build ./mlwsvm
#   make test         run the assembly test suite and the examples
#   make check-tools  smoke-test --monitor, top, --profile and --trace
#   make check-gc     run the tests with a garbage collection at every allocation
#   make check-asan   build with AddressSanitizer + UBSan and run the tests (normal and GC stress)
#   make check-switch build with switch dispatch instead of threaded code (as MSVC would) and run the tests
#   make check-loop   build with the threaded dispatch loop instead of tail calls (as GCC before 15 would) and run the tests
#   make check-poison build with every new register window filled with garbage before the
#                     registers a call clears are cleared, and run the tests (checks verify.c's plan)
#   make check-tsan   build with ThreadSanitizer and run the tests on several worker threads
#   make lib          the embedding library: build/libmlwsvm.a and a shared libmlwsvm
#   make check-embed  build and run the embedding example (examples/embed/host.c)
#   make test-ext     build the example extension the tests load (extensions/)
#   make embed-example  build the embedding example without running it
#   make check-bindgen  generate bindings for tests/_bindgen/libc.h, compare and call them (needs clang)
#   make bench        run the benchmarks in bench/
#   make docs         regenerate docs/ISA.md and docs/NATIVES.md from the code
#   make unicode UCD=DIR  regenerate the Unicode tables and conformance test data from an
#                     unpacked UCD.zip (https://www.unicode.org/Public/<version>/ucd/)

CC      ?= cc
CFLAGS  ?= -O2 -g
WARN    := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter
BUILD   ?= build
BIN     ?= mlwsvm

SRC := $(wildcard src/*.c src/*/*.c)

# A collection at every allocation is quadratic in a growing heap: the HTTP
# example takes about 3 minutes under AddressSanitizer, and longer on slow CI
# machines, so the GC-stress runs get more than the default 300 s per test.
STRESS_TIMEOUT := 1200

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

.PHONY: all test test-ext test-libs check-gc check-tools check-asan check-tsan check-switch check-loop check-poison check-bindgen bench docs unicode clean lib embed-example check-embed

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) -lm $(FFI_LIBS) $(DL_LIBS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(WARN) $(CFLAGS) -fPIC $(FFI_CFLAGS) -MMD -MP -c $< -o $@

# The web dashboard is compiled into the binary (served by --monitor).
$(BUILD)/embed: build-tools/embed.c
	@mkdir -p $(dir $@)
	$(CC) -O1 -o $@ $<

$(BUILD)/gen/dashboard.c: web/dashboard.html $(BUILD)/embed
	@mkdir -p $(dir $@)
	$(BUILD)/embed web/dashboard.html DASHBOARD_HTML > $@

$(BUILD)/gen/dashboard.o: $(BUILD)/gen/dashboard.c
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

# The embedding library (include/mlwsvm/embed.h): everything but the command
# line (main, and the test runner, doc generator, top and ext commands).
CLI_OBJ := $(addprefix $(BUILD)/src/,main.o tools/testrunner.o tools/doc.o tools/top.o tools/ext.o)
LIB_OBJ := $(filter-out $(CLI_OBJ),$(OBJ))
LIB_LIBS := -lm $(FFI_LIBS) $(DL_LIBS) -lpthread
ifeq ($(shell uname -s),Darwin)
SHLIB := $(BUILD)/libmlwsvm.dylib
SHLIB_FLAGS := -dynamiclib -install_name @rpath/libmlwsvm.dylib
else
SHLIB := $(BUILD)/libmlwsvm.so
SHLIB_FLAGS := -shared
endif

lib: $(BUILD)/libmlwsvm.a $(SHLIB)
	@echo "embed with: -I include and $(BUILD)/libmlwsvm.a $(LIB_LIBS)  (or $(SHLIB))"

$(BUILD)/libmlwsvm.a: $(LIB_OBJ)
	@rm -f $@
	ar rcs $@ $(LIB_OBJ)

$(SHLIB): $(LIB_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SHLIB_FLAGS) -o $@ $(LIB_OBJ) $(LIB_LIBS)

$(BUILD)/embed-host: examples/embed/host.c include/mlwsvm/embed.h include/mlwsvm/mlwsvm.h $(BUILD)/libmlwsvm.a
	$(CC) -std=c11 -Wall -Wextra -Wpedantic $(CFLAGS) $(LDFLAGS) -Iinclude -o $@ $< $(BUILD)/libmlwsvm.a $(LIB_LIBS)

embed-example: $(BUILD)/embed-host

check-embed: $(BUILD)/embed-host
	./$(BUILD)/embed-host lib

-include $(OBJ:.o=.d)

# Native extensions: the demo (extensions/demo) is the example and the test fixture.
EXT_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2 -g -fPIC -shared -pthread -Iinclude
build/ext/demo.mlwsx: extensions/demo/demo.c include/mlwsvm/mlwsvm.h
	@mkdir -p $(dir $@)
	$(CC) $(EXT_CFLAGS) -o $@ $<

# Trusted extension directories for tests/native: a good one, and three that
# must be refused (not in TRUSTED, changed since installed, writable by others).
TEST_EXT := build/test-ext
# The library archives the tests import (tests/libs): one per tests/_libs/NAME-VERSION.
TEST_LIBS := build/test-libs
test-libs: $(BIN)
	@rm -rf $(TEST_LIBS) && mkdir -p $(TEST_LIBS)
	@for d in tests/_libs/*/; do ./$(BIN) lib build $$d -o $(TEST_LIBS)/$$(basename $$d).mlwsl >/dev/null || exit 1; done

test-ext: $(BIN) build/ext/demo.mlwsx test-libs
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

# Generate bindings for tests/_bindgen/libc.h (needs clang), compare them with
# the expected output, and call them.
check-bindgen: $(BIN) test-ext
	@mkdir -p build/bindgen
	@if ! command -v clang >/dev/null 2>&1; then echo "check-bindgen: skipped (no clang)"; exit 0; fi; \
	./$(BIN) bindgen -l "" -p c -o build/bindgen/libc.inc tests/_bindgen/libc.h 2>/dev/null && \
	diff -u tests/_bindgen/libc.expected build/bindgen/libc.inc && \
	./$(BIN) run -L build/bindgen --ext-path $(TEST_EXT) tests/_bindgen/use.masm && \
	echo "check-bindgen: OK"

check-gc: $(BIN) test-ext
	MLWSVM_TEST_TIMEOUT=$(STRESS_TIMEOUT) MLWSVM_GC_STRESS=1 ./$(BIN) test tests examples

check-asan: test-ext
	$(MAKE) BUILD=build-asan BIN=mlwsvm-asan \
	    CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" \
	    LDFLAGS="-fsanitize=address,undefined"
	$(ASAN_ENV) ./mlwsvm-asan test tests examples
	$(ASAN_ENV) MLWSVM_TEST_TIMEOUT=$(STRESS_TIMEOUT) MLWSVM_GC_STRESS=1 ./mlwsvm-asan test tests examples
	$(MAKE) BUILD=build-asan BIN=mlwsvm-asan \
	    CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" \
	    LDFLAGS="-fsanitize=address,undefined" build-asan/embed-host
	$(ASAN_ENV) ./build-asan/embed-host lib

# The interpreter's switch dispatch, which compilers without computed goto use.
check-switch: test-ext
	$(MAKE) BUILD=build-switch BIN=mlwsvm-switch CFLAGS="$(CFLAGS) -DMLWS_NO_THREADED"
	./mlwsvm-switch test tests examples

check-loop: test-ext
	$(MAKE) BUILD=build-loop BIN=mlwsvm-loop CFLAGS="$(CFLAGS) -DMLWS_NO_TAILCALL"
	./mlwsvm-loop test tests examples

# What a call clears of a new window (verify.c, plan_window): with the rest
# full of garbage, any register read before it is written shows.
check-poison: test-ext
	$(MAKE) BUILD=build-poison BIN=mlwsvm-poison CFLAGS="$(CFLAGS) -DMLWS_POISON_WINDOWS"
	./mlwsvm-poison test tests examples

check-tsan: test-ext
	$(MAKE) BUILD=build-tsan BIN=mlwsvm-tsan \
	    CFLAGS="-O1 -g -fsanitize=thread -fno-omit-frame-pointer" LDFLAGS="-fsanitize=thread"
	TSAN_OPTIONS=halt_on_error=1 MLWSVM_THREADS=4 ./mlwsvm-tsan test tests examples
	TSAN_OPTIONS=halt_on_error=1 MLWSVM_THREADS=8 ./mlwsvm-tsan test tests/processes tests/threads tests/native tests/os tests/net
	$(MAKE) BUILD=build-tsan BIN=mlwsvm-tsan \
	    CFLAGS="-O1 -g -fsanitize=thread -fno-omit-frame-pointer" LDFLAGS="-fsanitize=thread" build-tsan/embed-host
	TSAN_OPTIONS=halt_on_error=1 ./build-tsan/embed-host lib

bench: $(BIN)
	sh bench/run.sh ./$(BIN)

docs: $(BIN)
	./$(BIN) doc isa -o docs/ISA.md
	./$(BIN) doc natives -o docs/NATIVES.md

# The Unicode Character Database's tables (src/core/unicode_tables.inc) and test
# data (tests/text/_unicode/), generated and committed, so a build needs neither.
unicode:
	@if [ -z "$(UCD)" ]; then echo "usage: make unicode UCD=dir (an unpacked UCD.zip)"; exit 2; fi
	python3 -I build-tools/gen_unicode.py "$(UCD)"

clean:
	rm -rf build build-asan build-tsan build-noffi build-switch build-loop build-poison mlwsvm mlwsvm-asan mlwsvm-tsan mlwsvm-noffi mlwsvm-switch mlwsvm-loop mlwsvm-poison
