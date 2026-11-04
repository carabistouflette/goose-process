GOOSE ?= .toolchain/goose/build/goose
CC ?= gcc
SANITIZER_CC ?= gcc
BENCH_ITERATIONS ?= 100
CFLAGS = -std=c11 -Wall -Wextra -Werror

.PHONY: setup format check test helpers native sanitize bench
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
helpers:
	mkdir -p .build tests/.build
	$(CC) $(CFLAGS) -O2 tests/helper.c -o .build/helper
	cp .build/helper tests/helper
test: helpers
	$(GOOSE) run tests/api.goose
	$(GOOSE) build tests/api.goose -o .build/api
	./.build/api
	$(GOOSE) build tests/api.goose --debug -o .build/api-debug
	./.build/api-debug
	$(GOOSE) run examples/command.goose
