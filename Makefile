# MLWSVM build. Plain C11; needs only libc and libm (plus -ldl and -lpthread on
# Linux). libffi is optional: when found, it enables the FFI (.extern).
#
#   make              build ./mlwsvm
#   make test         run the assembly test suite and the examples
#   make check-tools  smoke-test --monitor, top, --profile and --trace
#   make check-gc     run the tests with a garbage collection at every allocation, and with
#                     each minor collection checked against a walk of the whole heap
#   make check-opt    run the tests with every module optimized (--opt all), also with GC stress
#   make check-jit    run the tests with the JIT: every function compiled and entered at every
#                     block (--jit all), also with GC stress and optimized; then hot functions
#   make check-asan   build with AddressSanitizer + UBSan and run the tests (normal, GC stress, JIT)
#                     and 100 fuzzed programs
#   make check-switch build with switch dispatch instead of threaded code (as MSVC would) and run the tests
#   make check-loop   build with the threaded dispatch loop instead of tail calls (as GCC before 15 would) and run the tests
#   make check-poison build with every new register window filled with garbage before the
#                     registers a call clears are cleared, and run the tests (checks verify.c's plan)
#   make check-tsan   build with ThreadSanitizer and run the tests on several worker threads
#   make check-fuzz   run 300 random programs in every mode against the interpreter (mlwsvm fuzz)
#   make release-check  every check above, longer fuzzing, and the compilers' suites in every
#                     mode if ../mlws-compilers is there (tests/release-check.sh): before a release
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
# Every function starts on a 64-byte boundary, so that a change to one handler
# does not move the others across cache lines: without it, unrelated programs
# take up to 5% more or less time from one build to the next.
ALIGN   := -falign-functions=64
BUILD   ?= build
# Windows (MSYS2's CLANG64 environment): programs end in .exe, and the
# libraries are Winsock and the random-number API.
ifeq ($(OS),Windows_NT)
EXE     := .exe
endif
BIN     ?= mlwsvm$(EXE)

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
# On Windows libffi is linked in, so that mlwsvm.exe needs no DLL of MSYS2's.
ifeq ($(OS),Windows_NT)
ifneq ($(FFI_LIBS),)
FFI_LIBS := $(shell pkg-config --variable=libdir libffi)/libffi.a
FFI_CFLAGS += -DFFI_STATIC_BUILD
endif
endif
ifneq ($(FFI_LIBS),)
FFI_CFLAGS += -DMLWS_HAVE_FFI
endif
endif
ifeq ($(shell uname -s),Linux)
DL_LIBS := -ldl -lpthread
else ifneq ($(OS),Windows_NT)
DL_LIBS := -lpthread
endif
ifeq ($(OS),Windows_NT)
DL_LIBS := -lws2_32 -lbcrypt
endif
OBJ := $(SRC:%.c=$(BUILD)/%.o) $(BUILD)/gen/dashboard.o

.PHONY: check-fuzz release-check check-jit check-opt all test test-ext test-libs check-gc check-tools check-asan check-tsan check-switch check-loop check-poison check-bindgen bench docs unicode clean lib embed-example check-embed

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) -lm $(FFI_LIBS) $(DL_LIBS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(WARN) $(ALIGN) $(CFLAGS) -fPIC $(FFI_CFLAGS) -MMD -MP -c $< -o $@

# The web dashboard is compiled into the binary (served by --monitor).
$(BUILD)/embed$(EXE): build-tools/embed.c
	@mkdir -p $(dir $@)
	$(CC) -O1 -o $@ $<

$(BUILD)/gen/dashboard.c: web/dashboard.html $(BUILD)/embed$(EXE)
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
else ifeq ($(OS),Windows_NT)
SHLIB := $(BUILD)/libmlwsvm.dll
SHLIB_FLAGS := -shared
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
ifeq ($(OS),Windows_NT)
EXT_CFLAGS += -static # (winpthreads linked in: the extension loads without MSYS2's DLLs)
endif
build/ext/demo.mlwsx: extensions/demo/demo.c include/mlwsvm/mlwsvm.h
	@mkdir -p $(dir $@)
	$(CC) $(EXT_CFLAGS) -o $@ $<

# Let every user write to a directory (Windows: an access control entry for Everyone).
ifeq ($(OS),Windows_NT)
WORLD_WRITABLE = MSYS2_ARG_CONV_EXCL='*' icacls $(1) /grant '*S-1-1-0:(OI)(CI)W' >/dev/null
else
WORLD_WRITABLE = chmod o+w $(1)
endif

# The Windows libraries the FFI tests call (C runtimes, Winsock).
ifeq ($(OS),Windows_NT)
WIN_FFI_LIBS := ucrtbase msvcrt ws2_32
endif

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
	@./$(BIN) ext allow-ffi --dir $(TEST_EXT) - libm libm.so.6 $(WIN_FFI_LIBS) >/dev/null
	@./$(BIN) ext allow-ffi --dir build/test-ext-unlisted libm >/dev/null
	@cp build/ext/demo.mlwsx build/test-ext-unlisted/
	@./$(BIN) ext install --dir build/test-ext-tampered build/ext/demo.mlwsx >/dev/null
	@printf 'x' >> build/test-ext-tampered/demo.mlwsx
	@./$(BIN) ext install --dir build/test-ext-open build/ext/demo.mlwsx >/dev/null
	@$(call WORLD_WRITABLE,build/test-ext-open)

test: $(BIN) test-ext
	./$(BIN) test tests examples

check-tools: $(BIN)
	sh tests/tools/check_tools.sh ./$(BIN)

# Generate bindings for tests/_bindgen/libc.h (needs clang), compare them with
# the expected output, and call them. Windows (LLP64, another C library) has
# a header of its own.
ifeq ($(OS),Windows_NT)
BINDGEN_DIR := tests/_bindgen/win
else
BINDGEN_DIR := tests/_bindgen
endif
check-bindgen: $(BIN) test-ext
	@mkdir -p build/bindgen
	@if ! command -v clang >/dev/null 2>&1; then echo "check-bindgen: skipped (no clang)"; exit 0; fi; \
	./$(BIN) bindgen -l "" -p c -o build/bindgen/libc.inc $(BINDGEN_DIR)/libc.h 2>/dev/null && \
	diff -u $(BINDGEN_DIR)/libc.expected build/bindgen/libc.inc && \
	./$(BIN) run -L build/bindgen --ext-path $(TEST_EXT) $(BINDGEN_DIR)/use.masm && \
	echo "check-bindgen: OK"

check-gc: $(BIN) test-ext
	MLWSVM_TEST_TIMEOUT=$(STRESS_TIMEOUT) MLWSVM_GC_STRESS=1 ./$(BIN) test tests examples
	MLWSVM_GC_VERIFY=1 ./$(BIN) test tests examples
	MLWSVM_GC_VERIFY=1 MLWSVM_JIT=all ./$(BIN) test tests examples

# The tests with every module optimized by every pass (tests that depend on
# what -O2, -O3 or tailcall change say `; no-opt:`), plainly and with GC
# stress.
check-opt: $(BIN) test-ext
	MLWSVM_OPT=all ./$(BIN) test tests examples
	MLWSVM_TEST_TIMEOUT=$(STRESS_TIMEOUT) MLWSVM_OPT=all MLWSVM_GC_STRESS=1 ./$(BIN) test tests examples

# The tests with the JIT (docs/JIT.md): every function compiled as it is
# loaded and its machine code entered at every block (--jit all), plainly,
# with GC stress and with every module optimized; then functions compiled
# once hot (--jit on) after two calls or loop iterations, also on one worker
# thread. In a build without a JIT they run in the interpreter.
check-jit: $(BIN) test-ext
	MLWSVM_JIT=all ./$(BIN) test tests examples
	MLWSVM_TEST_TIMEOUT=$(STRESS_TIMEOUT) MLWSVM_JIT=all MLWSVM_GC_STRESS=1 ./$(BIN) test tests examples
	MLWSVM_JIT=all MLWSVM_OPT=all ./$(BIN) test tests examples
	MLWSVM_JIT=on MLWSVM_JIT_HOT=2 ./$(BIN) test tests examples
	MLWSVM_JIT=on MLWSVM_JIT_HOT=2 MLWSVM_THREADS=1 ./$(BIN) test tests examples

# Random programs (src/tools/fuzz.c), each run in the interpreter and in
# every other mode, which must print the same; seeds 1-300 every time.
check-fuzz: $(BIN)
	./$(BIN) fuzz -n 300 --seed 1 --out build/fuzz

release-check:
	sh tests/release-check.sh -c $(CC)

# AddressSanitizer and UBSan. One run also uses the chunk allocator, which
# ASan builds otherwise leave out (MLWSVM_SLABS=1): the leak check then sees
# any buffer of a dead object that the collector failed to free. The JIT runs
# both ways: every function compiled at once (--jit all), and each once hot,
# when its closure calls are compiled for the functions they called.
check-asan: test-ext
	$(MAKE) BUILD=build-asan BIN=mlwsvm-asan \
	    CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" \
	    LDFLAGS="-fsanitize=address,undefined"
	$(ASAN_ENV) ./mlwsvm-asan test tests examples
	$(ASAN_ENV) MLWSVM_TEST_TIMEOUT=$(STRESS_TIMEOUT) MLWSVM_GC_STRESS=1 ./mlwsvm-asan test tests examples
	$(ASAN_ENV) MLWSVM_JIT=all ./mlwsvm-asan test tests examples
	$(ASAN_ENV) MLWSVM_SLABS=1 MLWSVM_JIT=all ./mlwsvm-asan test tests examples
	$(ASAN_ENV) MLWSVM_JIT=on MLWSVM_JIT_HOT=2 ./mlwsvm-asan test tests examples
	$(ASAN_ENV) ./mlwsvm-asan fuzz -n 100 --seed 1 --out build/fuzz-asan
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
	TSAN_OPTIONS=halt_on_error=1 MLWSVM_THREADS=8 ./mlwsvm-tsan test tests/processes tests/threads tests/native tests/os tests/net tests/libs
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
