GOOSE ?= .toolchain/goose/build/goose
CC ?= gcc
SANITIZER_CC ?= gcc
BENCH_ITERATIONS ?= 100
BENCH_CC ?= gcc
FUZZ_CC ?= clang
FUZZ_RUNS ?= 10000
NATIVE_SOURCES = native/process.c native/launch.c
CFLAGS = -std=c11 -Wall -Wextra -Werror
DEADLINE_WRAPS = -Wl,--wrap=clock_gettime -Wl,--wrap=poll -Wl,--wrap=__poll_chk -Wl,--wrap=posix_spawn
LIFECYCLE_WRAPS = -Wl,--wrap=clock_gettime,--wrap=poll,--wrap=__poll_chk,--wrap=syscall,--wrap=kill
EXTENDED_WRAPS = -Wl,--wrap=socketpair,--wrap=shutdown,--wrap=send
CALLER_WRAPS = -Wl,--wrap=syscall,--wrap=kill,--wrap=waitpid

.PHONY: setup format check test helpers native sanitize bench compare compare-check fuzz profile memcheck
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
	$(GOOSE) check examples/compiler.goose
	$(GOOSE) check tests/features.goose
	$(GOOSE) check tests/stream.goose
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
	$(GOOSE) run tests/features.goose
	$(GOOSE) build tests/api.goose --cc $(CC) -o .build/api
	./.build/api
	$(GOOSE) build tests/api.goose --cc $(CC) --debug -o .build/api-debug
	./.build/api-debug
	$(GOOSE) build tests/features.goose --cc $(CC) -o .build/features
	./.build/features
	$(GOOSE) build tests/features.goose --cc $(CC) --debug -o .build/features-debug
	./.build/features-debug
	$(GOOSE) run tests/stream.goose
	$(GOOSE) build tests/stream.goose --cc $(CC) -O2 -o .build/stream
	./.build/stream
	$(GOOSE) build tests/stream.goose --cc $(CC) --debug --no-bce -o .build/stream-debug
	./.build/stream-debug
	$(GOOSE) run examples/command.goose
	$(GOOSE) run examples/compiler.goose
	python3 tests/ownership.py $(abspath $(GOOSE))
	$(MAKE) native
native: helpers
	$(CC) $(CFLAGS) -O2 tests/stream.c $(NATIVE_SOURCES) -Wl,--wrap=read -o .build/stream-native
	python3 scripts/run.py .build/stream-native
	$(CC) $(CFLAGS) -O2 tests/caller.c $(NATIVE_SOURCES) $(CALLER_WRAPS) -o .build/caller
	python3 scripts/run.py .build/caller
	$(CC) $(CFLAGS) -O2 tests/extended.c $(NATIVE_SOURCES) $(EXTENDED_WRAPS) -o .build/extended
	python3 scripts/run.py .build/extended
	$(CC) $(CFLAGS) -O2 tests/lifecycle.c $(NATIVE_SOURCES) $(LIFECYCLE_WRAPS) -o .build/lifecycle
	python3 scripts/run.py .build/lifecycle
	$(CC) $(CFLAGS) -O2 -pthread tests/native.c $(NATIVE_SOURCES) -o .build/native
	python3 scripts/run.py .build/native $(abspath .build/helper)
	$(CC) $(CFLAGS) -O2 tests/failures.c $(NATIVE_SOURCES) -Wl,--wrap=syscall -Wl,--wrap=kill -Wl,--wrap=waitpid -o .build/failures
	python3 scripts/run.py .build/failures
	$(CC) $(CFLAGS) -O2 tests/syscalls.c $(NATIVE_SOURCES) @tests/syscalls.wraps -o .build/syscalls-test
	python3 scripts/run.py .build/syscalls-test $(abspath .build/helper)
	$(CC) $(CFLAGS) -O2 tests/deadlines.c $(NATIVE_SOURCES) $(DEADLINE_WRAPS) -o .build/deadlines
	python3 scripts/run.py .build/deadlines
	$(CC) $(CFLAGS) -O2 -shared -fPIC $(NATIVE_SOURCES) -o .build/process.so
	python3 tests/model.py
	$(CC) $(CFLAGS) -O2 -shared -fPIC tests/validation.c $(NATIVE_SOURCES) -Wl,--wrap=posix_spawn -Wl,--wrap=clock_gettime -o .build/validation.so
	python3 tests/validation.py
sanitize: helpers
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie tests/stream.c $(NATIVE_SOURCES) -Wl,--wrap=read -o .build/stream-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 scripts/run.py .build/stream-sanitize
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie tests/caller.c $(NATIVE_SOURCES) $(CALLER_WRAPS) -o .build/caller-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 scripts/run.py .build/caller-sanitize
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie tests/extended.c $(NATIVE_SOURCES) $(EXTENDED_WRAPS) -o .build/extended-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 scripts/run.py .build/extended-sanitize
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie tests/lifecycle.c $(NATIVE_SOURCES) $(LIFECYCLE_WRAPS) -o .build/lifecycle-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 scripts/run.py .build/lifecycle-sanitize
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie -pthread tests/native.c $(NATIVE_SOURCES) -o .build/native-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 scripts/run.py .build/native-sanitize $(abspath .build/helper)
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie tests/failures.c $(NATIVE_SOURCES) -Wl,--wrap=syscall -Wl,--wrap=kill -Wl,--wrap=waitpid -o .build/failures-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 scripts/run.py .build/failures-sanitize
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie tests/syscalls.c $(NATIVE_SOURCES) @tests/syscalls.wraps -o .build/syscalls-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 scripts/run.py .build/syscalls-sanitize $(abspath .build/helper)
	$(SANITIZER_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -no-pie tests/deadlines.c $(NATIVE_SOURCES) $(DEADLINE_WRAPS) -o .build/deadlines-sanitize
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 scripts/run.py .build/deadlines-sanitize
bench: helpers
	python3 scripts/native-manifest.py --check
	mkdir -p .build
	$(GOOSE) build bench/bench.goose --cc $(BENCH_CC) -O2 -o .build/bench
	python3 bench/measure.py --goose $(abspath $(GOOSE)) --cc $(BENCH_CC) --iterations $(BENCH_ITERATIONS)
compare-check:
	python3 bench/compare/measure.py --goose $(abspath $(GOOSE)) --cc $(BENCH_CC) --build-only
compare:
	python3 bench/compare/measure.py --goose $(abspath $(GOOSE)) --cc $(BENCH_CC)
fuzz:
	mkdir -p .build
	python3 scripts/fuzz-seeds.py
	$(FUZZ_CC) $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined tests/fuzz.c $(NATIVE_SOURCES) -Wl,--wrap=posix_spawn -o .build/fuzz
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 .build/fuzz -runs=$(FUZZ_RUNS) -seed=3025 -max_len=1048704 -timeout=5 -artifact_prefix=.build/ .build/fuzz-corpus
profile: helpers
	$(CC) $(CFLAGS) -O2 bench/profile.c $(NATIVE_SOURCES) -o .build/profile
	strace -c -e trace=wait4,read,poll -o .build/syscalls.txt .build/profile $(abspath .build/helper)
memcheck: helpers
	$(CC) $(CFLAGS) -O1 -g bench/profile.c $(NATIVE_SOURCES) -o .build/memcheck
	python3 scripts/run.py valgrind --tool=memcheck --error-exitcode=99 --track-origins=yes --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=definite,indirect,possible --child-silent-after-fork=yes --log-file=.build/memcheck.log .build/memcheck $(abspath .build/helper)

.PHONY: checker-setup checker-consumer
checker-setup:
	python3 scripts/checker_consumer.py --goose "$(GOOSE)" --cc "$(CC)" --setup
checker-consumer:
	python3 scripts/checker_consumer.py --goose "$(GOOSE)" --cc "$(CC)"
.PHONY: checker-source-test checker-overhead
checker-source-test:
	python3 tests/checker_setup.py
	python3 tests/checker_controller.py
checker-overhead:
	python3 bench/checker_overhead.py --goose "$(GOOSE)" --cc "$(CC)"
