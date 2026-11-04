GOOSE ?= .toolchain/goose/build/goose
CC ?= gcc
SANITIZER_CC ?= gcc
BENCH_ITERATIONS ?= 100
BENCH_CC ?= gcc
FUZZ_CC ?= clang
FUZZ_RUNS ?= 10000
CFLAGS = -std=c11 -Wall -Wextra -Werror
DEADLINE_WRAPS = -Wl,--wrap=clock_gettime -Wl,--wrap=poll -Wl,--wrap=posix_spawn

.PHONY: setup format check test helpers native sanitize bench fuzz profile
setup:
	./scripts/setup-goose.sh
format:
	python3 scripts/native-manifest.py
	$(GOOSE) format process.goose tests examples bench
check:
	python3 scripts/native-manifest.py --check
	$(GOOSE) format --check process.goose tests examples bench
	$(GOOSE) check tests/api.goose
	$(GOOSE) check examples/command.goose
	$(GOOSE) check bench/bench.goose
helpers:
	mkdir -p .build
	$(CC) $(CFLAGS) -O2 tests/helper.c -o .build/helper.new
	mv .build/helper.new .build/helper
	cp .build/helper tests/helper.new
	mv tests/helper.new tests/helper
test: helpers
	python3 scripts/native-manifest.py --check
	$(GOOSE) run tests/api.goose
	$(GOOSE) build tests/api.goose --cc $(CC) -o .build/api
	./.build/api
	$(GOOSE) build tests/api.goose --cc $(CC) --debug -o .build/api-debug
	./.build/api-debug
	$(GOOSE) run examples/command.goose
	python3 tests/ownership.py $(abspath $(GOOSE))
	$(MAKE) native
native: helpers
	$(CC) $(CFLAGS) -O2 -pthread tests/native.c native/process.c -o .build/native
	python3 scripts/run.py .build/native $(abspath .build/helper)
	$(CC) $(CFLAGS) -O2 tests/failures.c native/process.c -Wl,--wrap=syscall -Wl,--wrap=kill -Wl,--wrap=waitpid -o .build/failures
	python3 scripts/run.py .build/failures
	$(CC) $(CFLAGS) -O2 tests/syscalls.c native/process.c @tests/syscalls.wraps -o .build/syscalls-test
	python3 scripts/run.py .build/syscalls-test $(abspath .build/helper)
	$(CC) $(CFLAGS) -O2 tests/deadlines.c native/process.c $(DEADLINE_WRAPS) -o .build/deadlines
	python3 scripts/run.py .build/deadlines
	$(CC) $(CFLAGS) -O2 -shared -fPIC native/process.c -o .build/process.so
	python3 tests/model.py
sanitize: helpers
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -pthread tests/native.c native/process.c -o .build/native-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 scripts/run.py .build/native-sanitize $(abspath .build/helper)
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie tests/failures.c native/process.c -Wl,--wrap=syscall -Wl,--wrap=kill -Wl,--wrap=waitpid -o .build/failures-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 scripts/run.py .build/failures-sanitize
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie tests/syscalls.c native/process.c @tests/syscalls.wraps -o .build/syscalls-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 scripts/run.py .build/syscalls-sanitize $(abspath .build/helper)
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie tests/deadlines.c native/process.c $(DEADLINE_WRAPS) -o .build/deadlines-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 scripts/run.py .build/deadlines-sanitize
bench: helpers
	python3 scripts/native-manifest.py --check
	mkdir -p .build
	$(GOOSE) build bench/bench.goose --cc $(BENCH_CC) -O2 -o .build/bench
	python3 bench/measure.py --goose $(abspath $(GOOSE)) --cc $(BENCH_CC) --iterations $(BENCH_ITERATIONS)
fuzz:
	mkdir -p .build
	python3 scripts/fuzz-seeds.py
	$(FUZZ_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined tests/fuzz.c native/process.c -Wl,--wrap=posix_spawn -o .build/fuzz
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 .build/fuzz -runs=$(FUZZ_RUNS) -seed=3025 -max_len=65557 -timeout=5 -artifact_prefix=.build/ .build/fuzz-corpus
profile: helpers
	$(CC) $(CFLAGS) -O2 bench/profile.c native/process.c -o .build/profile
	strace -c -e trace=wait4,read,poll -o .build/syscalls.txt .build/profile $(abspath .build/helper)
